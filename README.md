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

## DIP switches da placa

A A1S V2.2 tem um DIP de 5 posições que funciona como **roteador dos GPIOs 13/14/15** — esses pinos do ESP32 são compartilhados entre o SD card, o botão KEY2 e o header JTAG. Como só uma função pode usar cada GPIO por vez, o DIP escolhe qual delas está fisicamente conectada. Para esse projeto usamos o SD; tudo que compartilhar GPIO13/15 com o SD deve estar OFF.

Mapeamento nesta unidade (a ordem física varia por revisão da placa, confirmar na sua antes):

| Pos | Rótulo | GPIO | Conflita com SD? | Estado |
|---:|---|---|---|---|
| 1 | KEY2 | GPIO13 | Sim | **OFF** |
| 2 | DATA3 | GPIO13 | — (essencial pro SD) | **ON** |
| 3 | CMD | GPIO15 | — (essencial pro SD) | **ON** |
| 4 | MTCK | GPIO13 | Sim | **OFF** |
| 5 | MTDO | GPIO15 | Sim | **OFF** |

Configuração final: **`OFF ON ON OFF OFF`**.

Notas práticas:
- **SD e JTAG não convivem.** Para depurar via JTAG é preciso reorganizar o DIP e perder o SD; por isso depuração aqui é feita só via serial USB.
- **KEY2 é sacrificado em troca do SD.** Por isso o botão de debug usa KEY3 (GPIO19), não KEY2.
- **Diagnóstico**: `[SD] begin FALHOU` no serial é, em 9 de cada 10 casos, DIP errado. Em segundo lugar, cartão mal encaixado ou não-FAT32.
- Em revisões mais antigas da placa, `DATA3`/`CMD` aparecem nas posições 1 e 4 (e o resto reorganizado). A regra continua: só essas duas em ON, todas as outras OFF. Detalhe técnico completo em `CLAUDE.md`.

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

### Descobrir a porta serial

O caminho da porta pode mudar entre boots, especialmente se você usa hubs USB ou desconecta a placa. O `platformio.ini` deste projeto fixa `upload_port = /dev/cu.usbserial-0001`, que é o nome típico no Mac com CH340/CP2102 — se a sua for diferente, ajuste o `.ini` ou descubra primeiro:

```bash
pio device list                            # cross-platform: lista todas as portas + descrição
ls /dev/cu.*                               # macOS: lista todos os devices serial
ls /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART 2>/dev/null   # macOS: filtra só USB-serial
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null   # Linux
```

No Windows, o caminho é `COM3`, `COM4`, etc. — visível no **Device Manager → Ports (COM & LPT)** ou via `pio device list`.

Se nenhum dispositivo aparece ao plugar a placa: cabo USB sem fios de dados (só carga) ou driver USB-serial faltando — ver "Pré-requisitos".

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
