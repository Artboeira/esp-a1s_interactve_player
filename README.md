# ESP32 A1S Audio Kit — Loop Player

Player de áudio simples com loop contínuo de arquivo MP3 via cartão SD, para o board **AI-Thinker ESP32 Audio Kit V2.2** com módulo **ESP-A1S**.

## Hardware

- **Board:** AI-Thinker ESP32 Audio Kit V2.2
- **Módulo:** ESP-A1S (ESP32 + codec ES8388)
- **SD Card:** SanDisk Ultra 32GB (FAT32)
- **Audio file:** `/loop.mp3` na raiz do SD

## Estado atual

- ✅ SD card inicializa (SD_MMC, modo 1-bit)
- ✅ Codec ES8388 inicializa via I2C (`0x10`)
- ✅ I2S TX manual via `driver/i2s.h` legada
- ✅ **Loop de `/loop.mp3` do SD → libhelix → I2S → ES8388 → headphone**
- ⏳ Próximo: relé 2 canais + botão físico; possivelmente OSC/MQTT via WiFi

`src/main.cpp` é o player de loop. Dependência única hoje: `pschatzmann/arduino-libhelix`. O init do codec mora todo no `main.cpp` — `esphome/ESP32-audioI2S` foi removida do projeto.

## Por que não tocava antes

Dois bugs combinados (detalhes em `CLAUDE.md`):

1. **DAC saía do reset mutado.** `REG 0x19` (DACCONTROL3) tem default `0x32` com bit 5 (`DACMute`) em 1. Sem limpar para `0x02`, o DAC ignora todos os samples — produzindo só o estalo da saída ligando.
2. **Registros de roteamento errados.** O código antigo escrevia em `REG 0x26..0x29` achando que eram "Mixer L1/L2/R1/R2"; eram, na verdade, seleção de input (LINSEL/RINSEL) e bypass de line-in. O roteamento DAC → Mixer real é `REG 0x27` (LD2LO) e `REG 0x2A` (RD2RO).

A lib `esphome/ESP32-audioI2S` não limpa o DACMute na init automática do ES8388 — por isso "estalo mas sem áudio".

## Build & run

```bash
pio run                       # compilar
pio run -t upload             # compilar + flashar
pio device monitor -b 115200  # serial
pio run -t upload -t monitor  # upload + monitor (fluxo de iteração)
```

Porta serial típica no Mac: `/dev/cu.usbserial-0001`. CLI do PlatformIO costuma estar em `~/.platformio/penv/bin/pio` (não no PATH global).

Ver `CLAUDE.md` para contexto técnico completo (mapa dos registros, fluxo do sinal, plano para o player MP3).
