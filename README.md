# ESP32 A1S — One-shot MP3 Player com Ímã e LED

Player de áudio para instalação interativa, rodando em **AI-Thinker ESP32 Audio Kit V2.2** (módulo **ESP-A1S** com codec **ES8388**). Cada toque no botão dispara um MP3 do começo, com saída no headphone; em paralelo, dois relés controlam um ímã (que acompanha o som) e o LED indicador do próprio botão.

## Hardware

| Item | Detalhe |
|---|---|
| Board | AI-Thinker ESP32 Audio Kit V2.2 (ESP-A1S + ES8388) |
| SD Card | qualquer cartão FAT32 com `/loop.mp3` na raiz (testado em SanDisk Ultra 32 GB) |
| Áudio | Saída pelo jack EARPHONES (LOUT1/ROUT1); o speaker da placa também toca em paralelo se PA_EN=HIGH |
| Botão debug | KEY3 onboard (GPIO19) — toggle play/stop |
| Botão externo | Microswitch SPDT: COM→GND, NO→GPIO22; LED interno do botão é alimentado via relé |
| Relé canal A | GPIO18 → IN; cabeado com o ímã via NC. Liga em PLAY, desliga em STOP. |
| Relé canal B | GPIO23 → IN; cabeado com o LED do microswitch via NC. Aceso em STOPPED; pisca por 3 s em PLAY e depois apaga. |
| Alimentação | A1S via USB ou fonte 5 V no header; **módulo relé com fonte 5 V dedicada** e GND comum com a A1S. |

DIP switches da placa (crítico pro SD funcionar): **`OFF ON ON OFF OFF`** nesta unidade — a ordem dos switches varia entre revisões; ver `CLAUDE.md` para detalhe.

## Comportamento

| Estado | Áudio | Ímã (GPIO18) | LED do botão (GPIO23) |
|---|---|---|---|
| **STOPPED** (boot ou pós-EOF) | mudo (DAC mute) | OFF | aceso constante |
| **PLAYING**, primeiros 3 s | tocando do início | ON | piscando 1 Hz (~3 piscadas) |
| **PLAYING**, após 3 s | continua tocando | ON | apagado fixo |
| **EOF do MP3** ou 2º toque | para com fade do buffer DMA | volta a OFF | volta a aceso |

Iniciar tocando direto após o boot: trocar `#define START_PLAYING false` para `true` no `src/main.cpp`.

## Por que esse projeto existe (e por que dá tanto trabalho)

A placa A1S é conhecida por ser difícil de fazer áudio sair pelo headphone via Arduino. O caminho que funciona é fazer **init manual do ES8388** via I2C — nenhuma das libs Arduino populares (`esphome/ESP32-audioI2S`, `schreibfaul1/ESP32-audioI2S`) limpa o bit `DACMute` (`REG 0x19`, default `0x32`), e por isso o codec inicializa, energiza a saída (gera um estalo audível no fone), mas o DAC fica mudo. A correção é `REG19 = 0x02` na init manual, e usar os registros corretos de roteamento (`REG 0x27` e `REG 0x2A`, não `0x26..0x29` como muitas instruções antigas sugerem). Ver `CLAUDE.md` para o root cause completo com tabela de registros e dump validado.

## Pré-requisitos

**Software:**
- **[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/methods/index.html)** (CLI) — instalação via `pip install platformio` ou pelo instalador oficial. A IDE (extensão PlatformIO no VSCode) também serve e cobre todos os comandos abaixo. No Mac, o binário costuma ficar em `~/.platformio/penv/bin/pio` e não entra no PATH global; criar alias é opcional.
- **Driver USB-serial** — a A1S usa um conversor CP2102 ou CH340 (depende do lote). Mac já reconhece nativamente o CP2102; se a porta `/dev/cu.usbserial-*` ou `/dev/cu.SLAB_USBtoUART` não aparecer ao plugar a placa, instale o driver do fabricante:
  - CP2102: <https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers>
  - CH340: <https://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html>
- **Toolchain ESP32** — instalado automaticamente pelo PlatformIO na primeira build (`espressif32@~6.x`, `framework-arduinoespressif32@v2.0.17`, toolchain xtensa). Sem ação manual.

**Hardware:**
- Placa AI-Thinker ESP32 Audio Kit V2.2 (módulo ESP-A1S com codec ES8388).
- Cabo USB **de dados** (cabos só de carga não enumeram a porta serial).
- Cartão microSD **FAT32** com o arquivo `loop.mp3` na raiz. Testado com SanDisk Ultra 32 GB.
- DIP switches da placa configurados corretamente (ver tabela em `CLAUDE.md`; resumo: `OFF ON ON OFF OFF` nesta unidade).
- **Para a parte do ímã + LED**: módulo relé 2 canais (com optoacoplador), fonte 5 V dedicada para o relé com GND comum à A1S, fonte 12 V para o circuito do ímã/LED, e um microswitch SPDT com LED interno.

## Build & run

```bash
pio run                       # compilar
pio run -t upload             # compilar + flashar
pio device monitor -b 115200  # serial
pio run -t upload -t monitor  # upload + monitor (fluxo de iteração)
```

Porta serial típica no Mac: `/dev/cu.usbserial-0001`. Se o `pio run -t upload` reclamar de porta inexistente, conferir cabo USB e drivers do conversor.

## Estrutura do projeto

```
src/main.cpp                  Player completo: ES8388 init, I2S, SD, Helix, relés, botões.
platformio.ini                board=esp32dev, framework=arduino, lib única: arduino-libhelix.
docs/main_mp3_loop.cpp.bak    Backup histórico (esphome/ESP32-audioI2S) — não restaurar.
CLAUDE.md                     Contexto técnico para futura iteração com Claude Code.
```

## Próximas etapas

- Controle remoto via OSC ou MQTT (WiFi) — sincronizar peças ou disparar via servidor.
- Trocar o cabeamento do ímã do NC → NO + `MAGNET_ON_NC=0` em instalação prolongada, para evitar desgaste da bobina (em STOPPED o relé do ímã fica permanentemente energizado para manter o ímã off).
