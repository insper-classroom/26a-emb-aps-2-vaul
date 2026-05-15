# APS 2 — Controle Picareta (Minecraft)

Documentação de planejamento da APS 2 de Computação Embarcada. O objetivo deste documento é registrar o escopo do projeto, decisões de hardware, protocolo de comunicação e a arquitetura de firmware antes da implementação.

---

## 1. Jogo

**Minecraft (Java Edition, PC).**

Escolhemos Minecraft porque as ações principais do jogo se encaixam naturalmente no formato de um controle em forma de martelo/picareta:

- A ação de "minerar" / "atacar" é a mais frequente do jogo e casa com um movimento físico vertical do controle (cima-baixo), o que abre espaço para usar o expert de IA reconhecendo o gesto.
- A movimentação de câmera é contínua e analógica — perfeita para um joystick.
- Pulo e pausa são entradas discretas — botões resolvem.
- O jogo aceita bem latências moderadas (não é um FPS competitivo), o que é compatível com Bluetooth serial.

## 2. Ideia do controle

O controle é um **martelo físico que já temos em posse**, simulando uma picareta de Minecraft. A ideia é abrir o corpo do martelo, embutir toda a eletrônica dentro (placa, bateria, IMU, HC-06, alto-falante) e expor os elementos de interação no cabo e corpo do martelo:

- **Joystick analógico** no topo do cabo, próximo à mão dominante (polegar).
- **Botão Power/Pause** (com LED RGB) na lateral da cabeça — também indica nível de bateria.
- **Botão Pular** na lateral oposta, posicionado para o dedo indicador.
- **Botão Macro** (com LED RGB) na parte lateral. 
- **Alto-falante** apontando para fora da cabeça do martelo, para dar o feedback sonoro da picaretada o mais próximo possível do "ponto de impacto".
- **IMU (MPU6050)** fixada rigidamente dentro da cabeça do martelo, para captar o gesto de "marteladar".

A interação central é: o jogador segura o controle como um martelo de verdade e faz o movimento de cima-para-baixo para minerar. Os botões resolvem ações discretas e o joystick controla a câmera.

> O diagrama mecânico/sketch com a posição dos componentes está no Figma do projeto (link no final do README).

## 3. Inputs e Outputs

### Inputs

| Componente | Tipo | Interface | Função |
|---|---|---|---|
| Joystick analógico (X/Y) | 2x analógico | ADC | Controlar câmera (mouse X/Y) |
| MPU6050 (acelerômetro + giroscópio) | Sensor de movimento | I2C | Detecção do gesto "picaretar" via IA |
| Botão Power/Pause (com LED RGB) | Digital | GPIO + IRQ | Liga/desliga sistema, pausa jogo, gera novo PIN BT (clique longo) |
| Botão Pular | Digital | GPIO + IRQ | Aciona pulo no jogo (espaço) |
| Botão Macro (com LED RGB) | Digital | GPIO + IRQ | Grava / reproduz macro |
| Divisor de tensão da bateria | Analógico | ADC | Leitura de tensão para monitoramento |

Os 4 botões digitais exigidos pela entrega mínima são cobertos como: Power, Pular, Macro e o possível botão integrado de seleção do joystick (push do eixo Z) — todos operam por **callback / interrupção**.

### Outputs

| Componente | Interface | Função |
|---|---|---|
| LED RGB do botão Power | PWM (3x) | Indicador de nível de bateria (verde > amarelo > vermelho) |
| LED RGB do botão Macro | PWM (3x) | FSM do macro: apagado (vazio) / azul (gravado) / vermelho (executando) |
| Alto-falante (via PWM + filtro RC + amplificador) | PWM + DMA | Feedback sonoro da picaretada |
| Módulo HC-06 | UART | Transmissão dos eventos do controle para o PC |

### Microcontrolador

**Raspberry Pi Pico 2 (RP2350).** Escolha definida pelos labs Expert do módulo: o modelo de IA do Edge Impulse é compilado especificamente para a RP2350 e o lab de Bluetooth usa a Pico com HC-06.

## 4. Protocolo

### Camada física: Bluetooth Serial (HC-06)

A comunicação com o PC é feita via **Bluetooth SPP** usando o módulo HC-06 (UART entre Pico 2 e HC-06, Bluetooth serial entre HC-06 e PC). O PC enxerga o controle como uma porta COM virtual.

### Pareamento

- **Clique longo (>2s) no botão Power** gera um novo PIN aleatório, que é configurado no HC-06 via comandos AT. O PIN é exibido apenas pela serial USB durante desenvolvimento (a equipe sabe qual é). Em uso normal o PIN fica fixo após o primeiro pareamento.

### Camada de aplicação: protocolo serial ASCII

A Pico envia mensagens curtas terminadas em `\n` para o PC. Um **script Python rodando no PC** lê essas mensagens da porta COM e as converte em eventos de teclado/mouse usando `pynput` (ou `pyautogui`), que o Minecraft interpreta como input normal.

Comandos definidos:

| Comando | Significado | Ação no PC |
|---|---|---|
| `JX:<valor>` | Eixo X do joystick (-100 a 100) | Mover mouse no eixo X proporcionalmente |
| `JY:<valor>` | Eixo Y do joystick (-100 a 100) | Mover mouse no eixo Y proporcionalmente |
| `JUMP` | Botão pular pressionado | Pressionar tecla `espaço` |
| `MINE` | IA detectou movimento de picaretar | Clique esquerdo do mouse (minerar) |
| `PAUSE` | Botão power (clique curto) | Pressionar tecla `ESC` |
| `MACRO_REC` | Início de gravação de macro | (apenas log no PC) |
| `MACRO_PLAY:<seq>` | Reprodução de macro | Reproduzir sequência de teclas |
| `BAT:<percent>` | Nível de bateria (telemetria) | (apenas log/HUD opcional no PC) |

### Política de envio

- Joystick: enviado a ~50 Hz (a cada 20 ms), apenas quando a leitura mudar acima de um threshold (evitar floodar a serial Bluetooth com ruído de ADC parado).
- Botões: enviados por evento (na borda da ISR, via fila → task BT).
- Picaretada: enviada por evento quando a task de IA classifica `updown` com confiança acima de um limiar.

## 5. Conceitos extras (além da entrega mínima)

- **Gerenciamento de bateria:** divisor de tensão na entrada do ADC mede a tensão da LiPo 1S. A cor do LED RGB do botão Power reflete o nível em três faixas (verde / amarelo / vermelho piscando).
- **Botão Macro:** uma máquina de estados com 3 estados (`IDLE` → `RECORDING` → `READY` → `PLAYING` → `READY` ...) controla a função e a cor do LED RGB. Permite gravar uma sequência de cliques/movimentos e reproduzir depois.
- **Áudio embarcado (Expert-1):** reaproveitamos o lab de áudio PWM com filtro passa-baixa RC e amplificador, reproduzindo o som de picareta batendo em pedra toda vez que uma picaretada é detectada.
- **Três experts integrados:** IA (Edge Impulse + MPU6050), Bluetooth (HC-06) e RTOS (FreeRTOS, sem variáveis globais — toda comunicação inter-task via filas e semáforos).
- **Design:** o martelo é um corpo de referência forte e bem reconhecível. O esforço fica em embutir a eletrônica sem comprometer a forma original.

## 6. Arquitetura de firmware (RTOS — alto nível)

O firmware é estruturado em FreeRTOS, sem variáveis globais. A descrição detalhada está no diagrama de blocos no FigJam; abaixo o resumo das responsabilidades:

### Tasks

- **`task_joystick`** — Lê os dois ADCs do joystick periodicamente, filtra ruído, e publica eventos de movimento na fila central.
- **`task_buttons_handler`** — Acorda via semáforos liberados pelas ISRs dos botões, aplica debounce em software e publica eventos na fila central. Também detecta clique curto vs. clique longo no botão Power.
- **`task_imu_ai`** — Lê a MPU6050 via I2C em taxa fixa, alimenta o buffer do classificador Edge Impulse, executa inferência localmente e, ao detectar `updown` com confiança alta, publica evento `MINE` na fila central e dispara o áudio.
- **`task_audio`** — Toca samples de áudio via PWM + DMA quando recebe um pedido pela sua própria fila (vinda da `task_imu_ai`).
- **`task_battery`** — Lê o ADC do divisor de tensão da bateria a cada poucos segundos, calcula a faixa e atualiza o LED RGB do Power via PWM.
- **`task_macro_fsm`** — Implementa a máquina de estados do botão de macro, controla o LED RGB do macro, e quando está reproduzindo injeta eventos na fila central.
- **`task_bluetooth_tx`** — Consome a fila central de eventos, formata as mensagens ASCII e envia pela UART para o HC-06.

### Filas

- **Fila central de eventos do controle** — produzida por todas as tasks de input (joystick, botões, IA, macro), consumida pela `task_bluetooth_tx`.
- **Fila de áudio** — produzida pela `task_imu_ai` (e pela `task_macro_fsm` se quisermos sons de macro), consumida pela `task_audio`.

### Semáforos

- Um semáforo binário por botão, liberados pelas respectivas ISRs e aguardados pela `task_buttons_handler`.
- Um semáforo opcional para sinalizar "amostra IMU pronta" da ISR do timer da `task_imu_ai`.

### ISRs

- ISR de GPIO para cada botão (Power, Pular, Macro) — apenas libera o semáforo correspondente.
- ISR do DMA do PWM de áudio — sinaliza fim de bloco para a `task_audio` carregar o próximo.

> Diagrama de blocos completo (tasks, filas, semáforos, ISRs e fluxos de dados) está no FigJam.

## 7. Diagramas

- **Diagrama de blocos do firmware (RTOS):** *link do FigJam a inserir*
- **Diagrama mecânico do controle:** *link do FigJam a inserir*
