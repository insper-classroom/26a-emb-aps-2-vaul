# CLAUDE.md — APS 2: Controle Picareta (Minecraft)

Contexto compartilhado do projeto para evitar reexplicação a cada sessão. Detalhes
completos de escopo/decisões estão no `README.md`; aqui fica o essencial operacional.

## Objetivo

Construir um **controle físico em forma de martelo/picareta** para jogar Minecraft
(Java Edition, PC). O jogador segura o martelo e faz o gesto de "marteladar" para
minerar; joystick controla a câmera; botões resolvem ações discretas (pular, pausar).
Projeto de Computação Embarcada que integra três experts: **IA** (Edge Impulse +
MPU6050), **Bluetooth** (HC-06) e **RTOS** (FreeRTOS, sem variáveis globais).

## Arquitetura em duas pontas

```
[Martelo: Pico 2] --UART--> [HC-06] ~~Bluetooth SPP~~> [PC: porta COM virtual] --> [terminal.py] --> input de teclado/mouse no Minecraft
```

1. **Firmware** (`main/`) roda na Raspberry Pi Pico 2 (RP2350) sob FreeRTOS. Lê
   joystick/botões/IMU e envia eventos pela UART → HC-06 → Bluetooth.
2. **Interface PC** (`python/terminal.py`) lê a porta COM virtual do HC-06,
   demultiplexa o stream e injeta teclado/mouse no jogo via `SendInput` (Win32).

## Protocolo do stream Bluetooth (CRÍTICO)

O **mesmo** stream serial carrega dois tipos de dado intercalados. O receptor
(`terminal.py::_process_control_bytes`) demultiplexa byte a byte:

- **Quadro de mouse (binário):** `0xFF` (sentinela) + `axis` (1 byte: 0=X, 1=Y) +
  `value` (2 bytes, **little-endian, signed**, faixa -127..127). Os 3 bytes após o
  `0xFF` são opacos — podem coincidir com `\n`, `0xFF`, etc., por isso NÃO podem ser
  lidos como ASCII enquanto o quadro não fecha.
- **Comando ASCII:** qualquer outra sequência, terminada em `\n`/`\r`. Comandos:
  `JUMP` (espaço), `MINE` (clique esq.), `PAUSE` (ESC).

Firmware: `joystick_send_axis` monta o quadro `0xFF`; `send_command` enfileira ASCII.
Ambos passam pela `xQueueTX` como `tx_msg_t` atômico (evita intercalar bytes de
quadros diferentes).

## Firmware — `main/main.c` (FreeRTOS)

Tasks ativas hoje (subconjunto do plano do README):
- `tx_task` — consome `xQueueTX` e escreve na UART do HC-06.
- `button_task` (GPIO 10) — clique longo gera novo PIN BT via comandos AT.
- `jump_task` (GPIO 15, SW do joystick) — envia `JUMP\n` na borda de pressão.
- `pause_task` (GPIO 14) — envia `PAUSE\n`.
- `joystick_task` (ADC GPIO 26/27) — lê eixos, filtra (média móvel N=5), aplica
  deadzone, envia ambos os eixos a ~100 Hz (modelo de velocidade, envia até 0).
- `oled_task` (I2C1, GPIO 2/3) — SSD1306 mostra o PIN atual.

Ainda **não** implementadas (planejadas no README): IA/MPU6050 (`MINE`), áudio PWM,
bateria/LED RGB, macro FSM. `mpu6050.h` existe mas não está integrado ao `main.c`.

Config: `HC06_NAME "ENZO-BLUETOOTH"`, `HC06_PIN "1234"`. Build via Pico SDK +
FreeRTOS (`CMakeLists.txt`, `FreeRTOS_Kernel_import.cmake`).

## Interface PC — `python/terminal.py`

GUI Tkinter (single-panel, só Bluetooth — a antiga comunicação USB/UART direta foi
removida). Responsabilidades:
- Conectar a uma porta COM (a do HC-06), baud padrão 115200.
- Demultiplexar o stream e, no **modo Controle** (default ligado), mover o cursor e
  acionar teclas/cliques no jogo.
- Movimento de câmera por **velocidade**: pacote atualiza `self._vel`; `_mover_loop`
  a 120 Hz integra em subpixels via `SetCursorPos` (sem aceleração de ponteiro).
  Slider "Sens." ajusta o ganho em tempo real.

### Por que `SendInput` por scancode + admin

Minecraft (GLFW/LWJGL) ignora teclas "virtuais" — só reconhece **scancodes de
hardware**. Por isso usamos `SendInput` com `KEYEVENTF_SCANCODE`, não `pyautogui`.
Além disso, o Windows (UIPI) descarta injeção sintética vinda de processo de
integridade menor que o jogo elevado → `_ensure_admin()` relança o script via UAC.
`SendInput` retornar 0 = bloqueado (rodar como administrador resolve).

### Dependências
`python/requirements.txt`: `pyserial` (porta COM), `pyautogui` (legado; injeção real
usa `ctypes`/Win32). `settings.json` persiste última porta/baud.

## Hardware

- **MCU:** Raspberry Pi Pico 2 (RP2350) — exigido pelo lab de IA (Edge Impulse).
- **Bluetooth:** HC-06 (SPP) via UART.
- **Sensor:** MPU6050 (acelerômetro+giroscópio) via I2C — gesto de picaretar.
- **Display:** SSD1306 OLED 128x32 via I2C1.
- **Joystick** analógico (ADC) + botões digitais (GPIO/IRQ).
- **PC alvo:** Windows (a injeção de input é Win32-específica).

## Convenções

- Comentários em português, sem acentuação obrigatória no firmware C (mas mantenha
  acentos corretos em prosa/markdown e na UI Python).
- Plataforma de desenvolvimento: **Windows + PowerShell**.
- **Política Git:** o Claude não executa comandos que mutam o repositório (commit,
  push, add, etc.) — só o usuário commita. Ver instruções globais.
