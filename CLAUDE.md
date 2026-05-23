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

Loop de MP3 no **ESP32 Audio Kit V2.2 (ESP-A1S)** com saída pelo jack **EARPHONES (headphone)**, a partir de cartão SD.

## Estado atual

- ✅ SD card inicializa (SD_MMC 1-bit, 400 kHz)
- ✅ Codec **ES8388** inicializa manualmente via I2C
- ✅ I2S TX direto via `driver/i2s.h` legada
- ✅ **Loop de `/loop.mp3` do SD → libhelix MP3 → I2S → ES8388 → headphone** (`src/main.cpp` atual)
- ⏳ Próximas etapas do projeto: relé 2 canais, botão físico, possivelmente OSC/MQTT via WiFi

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

## Pipeline atual (MP3 loop)

```
SD_MMC (1-bit)  →  /loop.mp3
  → leitura em chunks de 4 KB
  → libhelix MP3DecoderHelix.write(chunk)
  → callback com PCM int16 entrelaçado L/R
  → i2s_write bloqueante (I2S0)
  → ES8388 (init manual no setup)
  → LOUT1/ROUT1 + LOUT2/ROUT2
```

Sample rate da I2S é ajustado no 1º frame decodificado (`i2s_set_clk` no callback do Helix). EOF → `audioFile.seek(0)` → loop perpétuo.

Pontos sensíveis para futuras mudanças:
- O callback do Helix roda síncrono dentro de `mp3.write()`. Bloquear demais ali trava o `loop()`.
- Mono MP3 é convertido para estéreo L=R em buffer estático (pra alimentar I2S em modo `RIGHT_LEFT`).
- O init do ES8388 (especialmente `REG19 = 0x02` para limpar DACMute) é a parte que QUALQUER outra lib de áudio na A1S costuma esquecer. Se trocar de lib no futuro, copie esse init para depois da init da lib.

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

Depois do loop MP3 estável:
- Relé de 2 canais (controle via GPIO).
- Botão físico de start/stop do loop.
- Possivelmente controle OSC/MQTT via WiFi (padrão das instalações).
