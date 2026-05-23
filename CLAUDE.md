# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> Fonte canônica. Há cópias-ponteiro em `~/Documents/Docs2026/estudioab/projetos/MNDS/RIO2C/` (pasta de documentação) que apontam para cá. Edite sempre este arquivo.

## Comandos comuns

```bash
pio run                          # compilar
pio run -t upload                # compilar + flashar via USB
pio device monitor -b 115200     # serial monitor
pio run -t upload -t monitor     # upload + monitor (fluxo de iteração)
pio run -t clean                 # limpar build
pio pkg update                   # atualizar libs depois de mexer em lib_deps
```

`board = esp32dev`, `framework = arduino`, framework-arduinoespressif32 v2.0.17 (ESP-IDF v4.x API legada `driver/i2s.h`). Porta serial típica no Mac: `/dev/cu.usbserial-0001`. Sem lint/formatter.

O CLI do PlatformIO costuma estar em `~/.platformio/penv/bin/pio` (não no PATH global).

## Objetivo

Player one-shot de MP3 no **ESP32 Audio Kit V2.2 (ESP-A1S)** com saída pelo jack **EARPHONES (headphone)**, a partir de cartão SD. Cada toque no botão (KEY3 onboard ou microswitch externo) dispara `/loop.mp3` do começo. Dois relés acompanham o playback — um liga um ímã e outro controla o LED do microswitch.

## Estado atual

- ✅ SD card inicializa (SD_MMC 1-bit, 400 kHz)
- ✅ Codec **ES8388** inicializa manualmente via I2C
- ✅ I2S TX direto via `driver/i2s.h` legada
- ✅ Playback `/loop.mp3` do SD → libhelix MP3 → I2S → ES8388 → headphone (`src/main.cpp`)
- ✅ Toggle start/stop via KEY3 onboard (GPIO19, debug) **e** microswitch externo (GPIO22) em paralelo
- ✅ EOF do MP3 → STOPPED (one-shot por toque)
- ✅ Relé canal A (GPIO18) → ímã, cabeado no NC → on em PLAY, off em STOP
- ✅ Relé canal B (GPIO23) → LED do microswitch, cabeado no NC → aceso em STOPPED, pisca 3 s e apaga em PLAY
- ⏳ Próxima etapa: possivelmente OSC/MQTT via WiFi (controle remoto / sincronização entre peças)

Dependências hoje no `platformio.ini`: apenas `pschatzmann/arduino-libhelix` (decoder MP3). A lib `esphome/ESP32-audioI2S` foi **removida** — ela não limpava o `DACMute` e era o motivo do "estalo sem áudio". O init manual do codec mora todo no `main.cpp`.

Backup do main antigo (que usava `Audio` da esphome) está em `docs/main_mp3_loop.cpp.bak` apenas como referência histórica — não restaurar.

## Root cause do "estalo mas sem áudio" (resolvido)

Duas coisas combinadas faziam o codec ficar com LOUT1/ROUT1 energizados (daí o estalo audível ao plugar fone) mas sem nenhum sample chegando ao analógico:

### 1. `DACMute` (REG 0x19, bit 5) sai do reset **ligado**

`REG19 (DACCONTROL3)` tem valor default `0x32` no power-on — bit 5 = `DACMute = 1`. Enquanto isso não for limpo, o DAC ignora tudo que vier pela I2S, mesmo com todas as saídas/mixers ativos. **A lib `esphome/ESP32-audioI2S 2.3.0` não limpa esse bit na init automática do ES8388** (provavelmente porque o fork não conhece a placa A1S por nome).

Correção: `es_write(0x19, 0x02)` na init manual.

### 2. Registros de roteamento DAC → Mixer estavam errados no código antigo

Versões anteriores do código (e o CLAUDE.md antigo) escreviam em `REG 0x26..0x29` achando que eram "Mixer L1/L2/R1/R2". **Não são.** Pelo datasheet do ES8388 e pelas inits de referência (pschatzmann/arduino-audiokit, espressif/esp-adf), o mapa correto é:

| Reg | Nome | Função |
|---|---|---|
| `0x26` | DACCONTROL16 | LINSEL / RINSEL — seleção de **input** (ADC). Não usar para roteamento DAC. |
| `0x27` | DACCONTROL17 | **LD2LO** — DAC L → LMixer. bit 7 = enable; bits 6:4 = gain (000=0 dB, 001=−3 dB, …). Para tocar: `0x90` (LD2LO=1, −3 dB) ou `0x80` (0 dB). |
| `0x28` | DACCONTROL18 | LI2LO — Line Input L → LMixer (bypass). |
| `0x29` | DACCONTROL19 | LI2LO gain. |
| `0x2A` | DACCONTROL20 | **RD2RO** — DAC R → RMixer. Mesmo formato de bit 7 + gain do 0x27. |
| `0x2D` | DACCONTROL23 | VROI — impedância da referência de saída. `0x00` (low) para headphone. |
| `0x2E` / `0x2F` | DACCONTROL24/25 | Volume **LOUT1 / ROUT1** (headphone). `0x1E` = 0 dB, `0x1F` = +1.5 dB, `0x00` = −45 dB. |
| `0x30` / `0x31` | DACCONTROL26/27 | Volume **LOUT2 / ROUT2** (speaker). |

Isso também explica o sintoma antigo "REG28 voltou 0x28, não 0x09 que escrevemos" — não havia rejeição: o registro de fato controla LI2LO (bypass de input), seu default reflete o estado da seleção de input, e ele nunca foi o registro que precisávamos mexer.

### Dump de registros válido (depois das correções, áudio sai)

```
REG00 (ChipCtrl1)       = 0x06   ← analog enable, sem Vmid reset
REG01 (ChipCtrl2)       = 0x50   ← VREF + IBIAS on
REG02 (ChipPwr)         = 0x00   ← DEM/STM/PLL up
REG03 (ADCPwr)          = 0xFF   ← ADC desligado (não usado)
REG04 (DACPwr)          = 0x3C   ← LOUT1+ROUT1+LOUT2+ROUT2 + LDAC+RDAC on
REG08 (MasterMode)      = 0x00   ← slave
REG17 (DACCONTROL1)     = 0x18   ← I2S, 16-bit
REG19 (DACCONTROL3)     = 0x02   ← DACMute LIMPO ← MUDANÇA-CHAVE
REG1C (DACCONTROL6)     = 0x00   ← unmuted (L/R)
REG27 (DACCONTROL17)    = 0x90   ← LD2LO=1, -3dB
REG2A (DACCONTROL20)    = 0x90   ← RD2RO=1, -3dB
REG2D (DACCONTROL23)    = 0x00   ← VROI low
REG2E / REG2F           = 0x1E   ← LOUT1/ROUT1 vol 0 dB
REG30 / REG31           = 0x1E   ← LOUT2/ROUT2 vol 0 dB
```

## Hardware

| Item | Detalhe |
|---|---|
| Board | AI-Thinker ESP32 Audio Kit V2.2 |
| Codec | **ES8388** (versões antigas usavam AC101 — esta NÃO é AC101) |
| I2C codec | SDA=GPIO33, SCL=GPIO32, addr **0x10** |
| I2S BCLK | GPIO27 |
| I2S LRC/WS | GPIO25 |
| I2S DOUT | GPIO26 |
| I2S MCLK | GPIO0 (via `PIN_CTRL = 0xFFF0` + `PERIPHS_IO_MUX_GPIO0_U → FUNC_GPIO0_CLK_OUT1`) |
| SD CLK | GPIO14 |
| SD CMD/MOSI | GPIO15 |
| SD D0/MISO | GPIO2 |
| SD D3/CS | GPIO13 (manter HIGH antes do `SD_MMC.begin`) |
| PA Enable (speaker amp) | GPIO21 — HIGH = liga amp. Não afeta o headphone. |
| Headphone detect | GPIO39 — LOW = headphone conectado. Software-only (não roteia sozinho). |
| Botão KEY3 (onboard, debug) | GPIO19 — pull-up físico na placa; pressionado = LOW |
| Microswitch externo (play/stop) | COM → GND do header; NO → GPIO22 (com `INPUT_PULLUP`). LED do microswitch é alimentado pelo relé (não conecta ao ESP32). |
| Relé canal A — ÍMÃ | IN → **GPIO18**. Ímã cabeado no **NC** do relé. Comportamento: STOPPED relé armado (NC aberto, ímã off); PLAY relé desarmado (NC fecha, ímã energizado). Lógica invertida no software por `#define MAGNET_ON_NC 1`. |
| Relé canal B — LED do microswitch | IN → **GPIO23**. LED+ → +12V; LED- → NC; COM → GND 12V. Relé desarmado → NC fecha → LED aceso. PLAY pisca por `LED_BLINK_DURATION_MS` e depois fica apagado até STOP/EOF. |
| Polaridade do módulo relé | **active-HIGH** (`#define RELAY_ACTIVE_LOW 0`) — testado em bancada. Se trocar de módulo, conferir o LED no estado STOPPED logo após boot: deve estar aceso. |
| Alimentação do relé | Fonte 5V dedicada para a bobina/opto; **GND da fonte 5V compartilhado com GND da A1S** (sem isso o optoacoplador não enxerga o nível). |
| Não usar | GPIO16/17 — reservados para PSRAM do ESP-A1S, não saem nos headers. |

### DIP switches (crítico para o SD)

DATA3 e CMD em **ON**, resto OFF: `ON - OFF - OFF - ON - OFF`.

## Fluxo do sinal validado

```
ESP32 i2s_write()
  → I2S0 (BCLK=27, LRC=25, DOUT=26, MCLK=0)
  → ES8388 DAC L/R   (I2S 16-bit, 44.1 kHz, slave)
  → REG27/2A         (DAC → LMixer/RMixer)
  → REG04            (LOUT1/ROUT1/LOUT2/ROUT2 enable)
  → REG2E..0x31      (volume = 0 dB)
  → pinos LOUT1/ROUT1  → jack EARPHONES
  → pinos LOUT2/ROUT2  → speaker (via PA / GPIO21=HIGH)
```

Headphone e speaker tocam simultaneamente nessa config (REG04 = 0x3C habilita os 4 outputs). Quem detecta fone fisicamente é GPIO39, mas o roteamento é por software — se quiser exclusividade, lê o GPIO39 e desabilita LOUT2/ROUT2 (`REG04 = 0xC0`).

## SD Card

SPI mode (`SD.h`) **não funciona** nesta placa. Usar `SD_MMC` em modo 1-bit:

```cpp
#include "SD_MMC.h"
#include "driver/gpio.h"

pinMode(13, OUTPUT);
digitalWrite(13, HIGH);          // D3 HIGH antes do init é crucial
delay(100);
gpio_set_pull_mode(GPIO_NUM_14, GPIO_PULLUP_ONLY);
gpio_set_pull_mode(GPIO_NUM_15, GPIO_PULLUP_ONLY);
gpio_set_pull_mode(GPIO_NUM_2,  GPIO_PULLUP_ONLY);
gpio_set_pull_mode(GPIO_NUM_13, GPIO_PULLUP_ONLY);
SD_MMC.setPins(14, 15, 2);
SD_MMC.begin("/sdcard", true, false, 400000);   // true=1-bit, 400kHz
```

## Pipeline atual

```
SD_MMC (1-bit)  →  /loop.mp3
  → leitura em chunks de 4 KB
  → libhelix MP3DecoderHelix.write(chunk)
  → callback com PCM int16 entrelaçado L/R
  → i2s_write bloqueante (I2S0)
  → ES8388 (init manual no setup)
  → LOUT1/ROUT1 + LOUT2/ROUT2
```

Sample rate da I2S é ajustado no 1º frame decodificado (`i2s_set_clk` no callback do Helix).

Pontos sensíveis para futuras mudanças:
- O callback do Helix roda síncrono dentro de `mp3.write()`. Bloquear demais ali trava o `loop()`.
- Mono MP3 é convertido para estéreo L=R em buffer estático (pra alimentar I2S em modo `RIGHT_LEFT`).
- O init do ES8388 (especialmente `REG19 = 0x02` para limpar DACMute) é a parte que QUALQUER outra lib de áudio na A1S costuma esquecer. Se trocar de lib no futuro, copie esse init para depois da init da lib.

## Estados do player e transições

Há **dois estados** (`bool playing`). Toda mudança passa por uma de duas funções; quando o relé e o LED do botão entrarem, **é dentro delas que se mexe** — nunca espalhar transições pelo `loop()`.

```
                ┌──────────────────────────────────┐
                │   STOPPED                        │
                │   - DAC mutado (REG19=32)        │
                │   - DMA buffer zerado            │
                │   - Ímã (GPIO18) OFF             │
                │   - LED (GPIO23) ACESO constante │
                └────────────┬─────────────────────┘
                             │ botão (KEY3 ou microswitch externo) flanco ↓
                             │   → playerStart()
                             │
                ┌────────────▼─────────────────────┐
                │   PLAYING                        │
                │   - DAC unmute (REG19=02)        │
                │   - decoder Helix resetado       │
                │   - audioFile.seek(0)            │
                │   - Ímã (GPIO18) ON durante toda a execução
                │   - LED (GPIO23): primeiros 3 s → piscando 1 Hz
                │                   após 3 s     → armado fixo → APAGADO
                └────────────┬─────────────────────┘
                             │ botão (KEY3 ou microswitch externo) flanco ↓
                             │ OU EOF do MP3
                             │   → playerStop()
                             └──→ volta a STOPPED
```

Constantes que controlam o LED em `main.cpp`:
- `LED_BLINK_DURATION_MS` (3000 hoje) — duração total da fase de blink após PLAY.
- `LED_BLINK_HALF_PERIOD_MS` (500 hoje) — meio período do blink, 500 ms ON / 500 ms OFF → 1 Hz visual, ~3 piscadas em 3 s.
- `MAGNET_ON_NC` (1 hoje) — ímã está no NC do relé; software inverte para que `magnetSet(true)` energize o ímã. Trocar fisicamente para NO + definir 0 reduz desgaste da bobina em instalações de longa duração.

**Pontos sensíveis:**
- `playerStop` espera o DMA buffer drenar (`delay(60)` no caminho EOF, antes do mute) para não cortar o final do áudio. No caminho via botão, mutar imediatamente é OK — o usuário quer silêncio agora.
- `playerStart` faz `mp3.end()` + `mp3.begin()` para zerar bytes parciais de frame que sobraram da execução anterior, antes do `seek(0)`. Sem isso, o 1º frame após restart vem corrompido.
- O estado inicial após boot é STOPPED (`#define START_PLAYING false`). Se quiser que a placa toque sozinha ao ligar, mude para `true`.
- **Ímã e LED são controlados separadamente.** `magnetSet()` / `ledSet()` são chamados de `playerStart` / `playerStop` para sincronizar com o áudio; `ledUpdate()` é chamado no `loop()` e cuida da máquina interna do LED (blink 3 s → apagado fixo).
- **Clique mecânico durante o blink**: a 1 Hz o relé do LED clica 3× em 3 s. Já é curto, mas se incomodar dá pra trocar por SSR ou cabear o LED via transistor num GPIO PWM (perde o canal de 12 V controlável, mas remove o clique).

## Plano B se algo der ruim

- **ESP-IDF direto** (sem Arduino) — mais controle sobre drivers I2S/I2C.
- **Squeezelite-ESP32** — firmware pronto, sabidamente funciona na A1S, mas perdemos liberdade pra integrar relé/botão/OSC.
- **SdFat em vez de SD_MMC** se aparecer conflito de FS.h ao integrar Helix.

## Referências usadas

- Datasheet ES8388 (Everest Semiconductor) — mapa real dos registros 0x17–0x31.
- pschatzmann/arduino-audiokit `es8388.c` — sequência de power-up e valores `0x90` para LD2LO/RD2RO.
- espressif/esp-adf — init de referência para ES8388 na placa A1S.
- https://www.pschatzmann.ch/home/2021/12/06/the-ai-thinker-audio-kit-experience-or-nothing-is-right/

## Próximas etapas do projeto

- Possivelmente controle OSC/MQTT via WiFi (padrão das instalações) — para sincronização entre peças ou controle remoto.
- Avaliar trocar o relé do ímã (NC → NO) em instalação prolongada para evitar desgaste da bobina, conforme nota em `MAGNET_ON_NC`.
