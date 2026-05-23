// Loop de /loop.mp3 do SD → decoder Helix → I2S → ES8388 → headphone+speaker.
// Mantém o init manual do ES8388 e do I2S que validamos no teste de tom 440 Hz.
//
// Cadeia:
//   SD_MMC (1-bit) → arquivo /loop.mp3
//     → libhelix MP3DecoderHelix (chunks de 4 KB)
//       → callback com PCM int16 entrelaçado L/R
//         → i2s_write bloqueante na I2S0
//           → ES8388 DAC → LMix/RMix → LOUT1/ROUT1 + LOUT2/ROUT2
//
// Sample rate da I2S é ajustado para o sampRate do MP3 na 1ª frame decodificada.
// Quando o arquivo termina, faz seek(0) e continua → loop perpétuo.

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
// Active-low típico de módulos chineses com optoacoplador.
// Canais separados (preparado para comportamentos distintos no futuro),
// hoje espelhados via relayWrite().
// Nota: GPIO16/17 são reservados para PSRAM do módulo ESP-A1S — NÃO saem
// nos headers, não usar.
//
// LED do microswitch (instalação atual):
//   LED+ → +12V (fonte externa, direto)
//   LED- → NC do relé canal 1
//   COM relé canal 1 → GND da fonte 12V
//   → Relé DESARMADO (IN=HIGH): NC fecha, LED ACESO
//   → Relé  ARMADO  (IN=LOW):  NO fecha, LED APAGADO
#define RELAY_IN1_PIN 23
#define RELAY_IN2_PIN 18
#define RELAY_ACTIVE_LOW 0    // este módulo respondeu como active-HIGH (testado 2026-05-23)
#define BLINK_HALF_PERIOD_MS 1000   // 1 s aceso / 1 s apagado
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

// ---------------- relays ----------------

static bool relayArmed = false;
static unsigned long relayLastFlip = 0;

static void relayWrite(bool armed) {
  int level = RELAY_ACTIVE_LOW ? (armed ? LOW : HIGH) : (armed ? HIGH : LOW);
  digitalWrite(RELAY_IN1_PIN, level);
  digitalWrite(RELAY_IN2_PIN, level);
  relayArmed = armed;
  relayLastFlip = millis();
}

// Chamada de player state: STOPPED → LED aceso (relé desarmado).
static void relayHoldStopped() {
  if (relayArmed) relayWrite(false);
}

// Chamada no loop: enquanto PLAYING, alterna a cada BLINK_HALF_PERIOD_MS.
static void relayUpdateBlink() {
  if (millis() - relayLastFlip >= BLINK_HALF_PERIOD_MS) {
    relayWrite(!relayArmed);
  }
}

// ---------------- player state ----------------

static bool playing = false;

static void playerStop() {
  if (!playing) return;
  playing = false;
  es_write(0x19, 0x32);                  // mute DAC (limpa pop)
  i2s_zero_dma_buffer(I2S_PORT);
  relayHoldStopped();                    // LED aceso constante
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
  relayWrite(true);                      // arma já: LED apaga imediatamente,
                                         // dando feedback visual instantâneo
  Serial.println("[player] START");
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

  // Relé: fixa o nível ANTES de virar OUTPUT para evitar pulso espúrio no boot.
  int idle = RELAY_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(RELAY_IN1_PIN, idle);
  digitalWrite(RELAY_IN2_PIN, idle);
  pinMode(RELAY_IN1_PIN, OUTPUT);
  pinMode(RELAY_IN2_PIN, OUTPUT);
  relayWrite(false);                     // estado seguro inicial: LED aceso

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

  if (!playing) {
    delay(5);
    return;
  }

  relayUpdateBlink();

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
