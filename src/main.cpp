// Player one-shot de /loop.mp3 para AI-Thinker ESP32 Audio Kit V2.2 (ESP-A1S).
// Cada toque no botão (KEY3 onboard ou microswitch externo) dispara o arquivo
// do começo. Tocar de novo durante a reprodução, ou chegar ao EOF, volta ao
// estado STOPPED. Em paralelo, dois relés controlam um ímã e o LED do botão.
//
// Cadeia de áudio:
//   SD_MMC (1-bit) → /loop.mp3
//     → libhelix MP3DecoderHelix (chunks de 4 KB)
//       → callback com PCM int16 entrelaçado L/R
//         → i2s_write bloqueante na I2S0
//           → ES8388 (init manual) DAC → LMix/RMix → LOUT1/ROUT1 + LOUT2/ROUT2
//
// Sample rate da I2S é ajustado para o sampRate do MP3 na 1ª frame decodificada.
// O DAC do ES8388 sai do reset MUTADO (REG19 default 0x32); é necessário
// escrever 0x02 para liberar áudio — esse era o motivo do antigo "estalo
// mas sem som". Ver CLAUDE.md para o root cause completo.

#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "soc/io_mux_reg.h"
#include "MP3DecoderHelix.h"

#define ES8388_ADDR  0x10
#define I2S_PORT     I2S_NUM_0
#define PIN_BCLK     27
#define PIN_LRC      25
#define PIN_DOUT     26
#define PIN_MCLK     0
#define PA_EN        21

// Botões de start/stop (ambos toggle, em paralelo):
//   KEY3 onboard da A1S V2.2 → GPIO19 (mantido para debug em bancada)
//   Microswitch externo COM→GND, NO→GPIO22 (com pull-up interno)
// Apertar qualquer um dispara playerToggle().
#define BUTTON_KEY3_PIN  19
#define BUTTON_EXT_PIN   22
#define BUTTON_DEBOUNCE_MS 30

// Relé externo 2 canais (alimentação 5V dedicada, GND comum com A1S).
// Módulo testado em bancada: active-HIGH (RELAY_ACTIVE_LOW=0).
//
// Mapeamento desta instalação:
//   ÍMÃ  →  GPIO18 → IN do canal cabeado em série com a bobina do ímã,
//           via NC (ver MAGNET_ON_NC abaixo). Liga em PLAY, desliga em STOP.
//   LED  →  GPIO23 → IN do canal do LED do microswitch.
//           LED+ → +12V; LED- → NC; COM canal → GND 12V.
//           relé DESARMADO → NC fecha → LED ACESO
//           relé ARMADO    → NO fecha → LED APAGADO
//           Em STOPPED: aceso constante. Em PLAY: pisca por LED_BLINK_DURATION_MS,
//           depois fica apagado até STOP/EOF.
//
// GPIO16/17 são reservados para PSRAM do módulo ESP-A1S — NÃO saem nos
// headers, não usar.
#define MAGNET_PIN 18
#define LED_PIN    23
#define RELAY_ACTIVE_LOW 0    // testado em bancada: este módulo é active-HIGH.

// Ímã cabeado no NC do relé: energiza naturalmente quando o relé está
// desarmado. Para inverter, mover o fio do ímã do NC pro NO no módulo e
// trocar este define para 0.
// Atenção: com MAGNET_ON_NC=1, em STOPPED o relé fica permanentemente
// armado para manter o ímã off — desgaste contínuo da bobina ao longo do
// tempo. Para instalação prolongada, prefira trocar para NO + MAGNET_ON_NC=0.
#define MAGNET_ON_NC 1

#define LED_BLINK_DURATION_MS    3000  // duração total da fase de blink após PLAY
#define LED_BLINK_HALF_PERIOD_MS 500   // 500 ms ON / 500 ms OFF → 1 Hz, ~3 piscadas em 3 s
// Inicia STOPPED (espera 1º toque). Mudar para true se quiser começar tocando.
#define START_PLAYING false

#define MP3_PATH     "/loop.mp3"
#define SD_BUF_BYTES 4096

using libhelix::MP3DecoderHelix;
// MP3FrameInfo é definida no escopo global pela lib helix-mp3 (C).

static uint32_t currentSampleRate = 44100;
static uint8_t  currentChannels   = 2;

// ---------------- ES8388 ----------------

static bool es_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8388_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static void initES8388() {
  es_write(0x00, 0x80); delay(10);
  es_write(0x00, 0x00);
  es_write(0x08, 0x00);              // slave
  es_write(0x02, 0xF3); delay(10);
  es_write(0x02, 0x00);
  es_write(0x01, 0x50);
  es_write(0x00, 0x06);
  es_write(0x03, 0xFF);              // ADC off
  es_write(0x04, 0x3C);              // LOUT1+ROUT1+LOUT2+ROUT2 + LDAC+RDAC on
  es_write(0x19, 0x02);              // ⚠ CLEAR DACMute (default 0x32 = muted)
  es_write(0x17, 0x18);              // I2S, 16-bit
  es_write(0x18, 0x02);
  es_write(0x1A, 0x00);              // DAC L digital vol 0 dB
  es_write(0x1B, 0x00);              // DAC R digital vol 0 dB
  es_write(0x1C, 0x00);              // unmute L/R
  es_write(0x27, 0x90);              // LD2LO=1, -3 dB
  es_write(0x2A, 0x90);              // RD2RO=1, -3 dB
  es_write(0x2D, 0x00);              // VROI low (bom p/ fone)
  es_write(0x2E, 0x1E);              // LOUT1 vol 0 dB
  es_write(0x2F, 0x1E);              // ROUT1 vol 0 dB
  es_write(0x30, 0x1E);              // LOUT2 vol 0 dB
  es_write(0x31, 0x1E);              // ROUT2 vol 0 dB
}

// ---------------- I2S ----------------

static void routeMCLKtoGPIO0() {
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_CLK_OUT1);
  REG_WRITE(PIN_CTRL, 0xFFF0);
}

static void initI2S(uint32_t sampleRate) {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = sampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = true;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRC;
  pins.data_out_num = PIN_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_set_clk(I2S_PORT, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  routeMCLKtoGPIO0();
}

// ---------------- SD ----------------

static void initSD() {
  pinMode(13, OUTPUT);
  digitalWrite(13, HIGH);
  delay(100);
  gpio_set_pull_mode(GPIO_NUM_14, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(GPIO_NUM_15, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(GPIO_NUM_2,  GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(GPIO_NUM_13, GPIO_PULLUP_ONLY);
  SD_MMC.setPins(14, 15, 2);
  if (!SD_MMC.begin("/sdcard", true, false, 400000)) {
    Serial.println("[SD] begin FALHOU — DIP switches DATA3+CMD ON?");
    while (true) delay(1000);
  }
  Serial.println("[SD] OK");
}

// ---------------- Helix decode callback ----------------

static void mp3DataCallback(MP3FrameInfo &info, int16_t *pwm, size_t lenSamples, void * /*ref*/) {
  // Reconfigura I2S se sample rate ou n_chans mudar
  if ((uint32_t)info.samprate != currentSampleRate ||
      (uint8_t)info.nChans   != currentChannels) {
    currentSampleRate = info.samprate;
    currentChannels   = info.nChans;
    Serial.printf("[mp3] %d Hz, %d ch, %d bps — reconfig I2S\n",
                  info.samprate, info.nChans, info.bitsPerSample);
    i2s_set_clk(I2S_PORT, info.samprate, I2S_BITS_PER_SAMPLE_16BIT,
                info.nChans == 1 ? I2S_CHANNEL_MONO : I2S_CHANNEL_STEREO);
  }

  // Helix entrega samples já entrelaçados L/R quando nChans=2.
  // Mono: duplica para L=R antes de mandar pra I2S estéreo.
  if (info.nChans == 2) {
    size_t bytes = lenSamples * sizeof(int16_t);
    size_t written = 0;
    i2s_write(I2S_PORT, pwm, bytes, &written, portMAX_DELAY);
  } else {
    static int16_t stereo[2304 * 2];
    size_t n = lenSamples;
    if (n > sizeof(stereo) / sizeof(stereo[0]) / 2) n = sizeof(stereo) / sizeof(stereo[0]) / 2;
    for (size_t i = 0; i < n; i++) {
      stereo[i*2]   = pwm[i];
      stereo[i*2+1] = pwm[i];
    }
    size_t written = 0;
    i2s_write(I2S_PORT, stereo, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
  }
}

static MP3DecoderHelix mp3(mp3DataCallback);
static File audioFile;
static uint8_t sdBuf[SD_BUF_BYTES];

// Declarada antes dos relés porque ledUpdate() consulta este estado.
static bool playing = false;

// ---------------- relays ----------------

static bool magnetArmed = false;
static bool ledArmed    = false;     // ledArmed=true  →  LED APAGADO (NC aberto)
                                     // ledArmed=false →  LED ACESO  (NC fechado)
static unsigned long playStartMs   = 0;
static unsigned long ledLastFlipMs = 0;

static int activeLevel(bool armed) {
  return RELAY_ACTIVE_LOW ? (armed ? LOW : HIGH) : (armed ? HIGH : LOW);
}

static void magnetSet(bool on) {
  // on = ímã energizado fisicamente. Se cabeado no NC, precisamos do
  // relé DESARMADO para fechar o circuito do ímã.
  bool relayArmed = MAGNET_ON_NC ? !on : on;
  digitalWrite(MAGNET_PIN, activeLevel(relayArmed));
  magnetArmed = on;
}

static void ledSet(bool armed) {
  digitalWrite(LED_PIN, activeLevel(armed));
  ledArmed = armed;
}

// Chamada continuamente no loop. Garante o estado do LED conforme a
// máquina de estados:
//   STOPPED                    → LED aceso (desarmado)
//   PLAYING, primeiros 3 s     → LED piscando 1 Hz
//   PLAYING, após 3 s          → LED apagado (armado fixo)
static void ledUpdate() {
  if (!playing) {
    if (ledArmed) ledSet(false);
    return;
  }
  unsigned long now = millis();
  unsigned long elapsed = now - playStartMs;
  if (elapsed < LED_BLINK_DURATION_MS) {
    if (now - ledLastFlipMs >= LED_BLINK_HALF_PERIOD_MS) {
      ledSet(!ledArmed);
      ledLastFlipMs = now;
    }
  } else {
    if (!ledArmed) {
      ledSet(true);
      Serial.println("[led] blink terminou, mantendo apagado");
    }
  }
}

// ---------------- player state ----------------

static void playerStop() {
  if (!playing) return;
  playing = false;
  es_write(0x19, 0x32);                  // mute DAC (limpa pop)
  i2s_zero_dma_buffer(I2S_PORT);
  magnetSet(false);                      // desliga ímã
  ledSet(false);                         // LED volta aceso constante
  Serial.println("[player] STOP");
}

static void playerStart() {
  if (playing) return;
  // Reseta decoder Helix (pode ter bytes parciais de frame de execução anterior)
  mp3.end();
  mp3.begin();
  audioFile.seek(0);
  es_write(0x19, 0x02);                  // unmute DAC
  playing = true;
  playStartMs = millis();
  ledLastFlipMs = playStartMs;
  magnetSet(true);                       // liga ímã imediatamente
  ledSet(true);                          // LED apaga já — 1ª meia-onda do blink
  Serial.println("[player] START (ímã ON, LED blink 3 s)");
}

static void playerToggle() {
  if (playing) playerStop(); else playerStart();
}

// ---------------- buttons ----------------

struct ButtonState {
  int pin;
  int lastReading;
  int lastStable;
  unsigned long lastChange;
};

static ButtonState btnKey3 = { BUTTON_KEY3_PIN, HIGH, HIGH, 0 };
static ButtonState btnExt  = { BUTTON_EXT_PIN,  HIGH, HIGH, 0 };

// Retorna true uma vez por flanco de descida (LOW = pressionado, com pull-up).
static bool pollOne(ButtonState &b) {
  int reading = digitalRead(b.pin);
  if (reading != b.lastReading) {
    b.lastChange = millis();
    b.lastReading = reading;
  }
  if ((millis() - b.lastChange) > BUTTON_DEBOUNCE_MS && reading != b.lastStable) {
    b.lastStable = reading;
    return (reading == LOW);
  }
  return false;
}

static void pollButtons() {
  if (pollOne(btnKey3)) { Serial.println("[btn] KEY3"); playerToggle(); }
  if (pollOne(btnExt))  { Serial.println("[btn] EXT (microswitch)"); playerToggle(); }
}

// ---------------- setup / loop ----------------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Loop MP3 (init manual ES8388 + Helix) ===");

  pinMode(PA_EN, OUTPUT);
  digitalWrite(PA_EN, HIGH);           // amp speaker ON (fone independe)

  Wire.begin(33, 32);
  Wire.setClock(100000);
  delay(50);
  initES8388();
  Serial.println("[ES8388] OK");

  initI2S(currentSampleRate);
  Serial.println("[I2S] OK (44.1 kHz inicial, ajusta no 1º frame MP3)");

  initSD();

  audioFile = SD_MMC.open(MP3_PATH);
  if (!audioFile) {
    Serial.printf("[SD] arquivo %s não encontrado\n", MP3_PATH);
    while (true) delay(1000);
  }
  Serial.printf("[SD] aberto %s (%u bytes)\n", MP3_PATH, (unsigned)audioFile.size());

  mp3.begin();
  Serial.println("[mp3] decoder pronto");

  pinMode(BUTTON_KEY3_PIN, INPUT_PULLUP);
  pinMode(BUTTON_EXT_PIN,  INPUT_PULLUP);

  // Relés: fixa o nível ANTES de virar OUTPUT para evitar pulso espúrio no boot.
  //  - Ímã: queremos OFF. Se cabeado no NC, isso exige o relé ARMADO.
  //  - LED: queremos ACESO. Cabeado no NC → relé DESARMADO.
  digitalWrite(MAGNET_PIN, activeLevel(MAGNET_ON_NC ? true : false));
  digitalWrite(LED_PIN,    activeLevel(false));
  pinMode(MAGNET_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);
  magnetSet(false);
  ledSet(false);

  // Estado inicial
  if (START_PLAYING) {
    playerStart();
  } else {
    es_write(0x19, 0x32);   // mute DAC até o 1º toque
    Serial.println("[player] STOPPED — aperte KEY3 ou microswitch externo para iniciar");
  }
}

void loop() {
  pollButtons();
  ledUpdate();

  if (!playing) {
    delay(5);
    return;
  }

  if (!audioFile.available()) {
    // DMA buffer atual = 8 × 256 frames a 44.1 kHz ≈ 46 ms; esperamos um pouco
    // mais para o final do MP3 sair limpo antes de mutar o DAC.
    Serial.println("[loop] EOF — drenando buffer e parando");
    delay(60);
    playerStop();
    return;
  }
  int n = audioFile.read(sdBuf, sizeof(sdBuf));
  if (n > 0) {
    mp3.write(sdBuf, (size_t)n);
  }
}
