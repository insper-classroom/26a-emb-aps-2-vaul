# Guia de Debug, Calibração e Teste — Controle Picareta (Minecraft)

Este documento reúne **tudo** o que é preciso para: compilar e gravar o firmware,
conectar (USB ou Bluetooth), abrir a interface PC, **calibrar a intensidade da
marretada**, usar o **macro**, e **testar de verdade no Minecraft**. É autocontido —
quem for testar/gravar o vídeo consegue seguir do zero por aqui.

> Contexto rápido: o controle em forma de martelo envia eventos (câmera, botões,
> marretada) por um stream serial. No PC, o `python/terminal.py` lê esse stream e
> injeta teclado/mouse no jogo. Detalhes de arquitetura: `README.md` e `CLAUDE.md`.

---

## 0. TL;DR (resumo do que importa)

- **A marretada (MINE) agora dispara por ENERGIA do movimento, não pelo modelo de IA.**
  O modelo do Edge Impulse se mostrou **não confiável** ao vivo (classificava
  "porrada 99%" com o martelo **parado** e "idle" em swings fortes). Em vez de confiar
  nele, o firmware mede o **pico-a-pico (pp)** da aceleração na janela e dispara o
  clique quando o movimento é forte o suficiente.
- **Limiar atual: `ENERGY_TH = 9000`** (em `main/gesture.cpp`). **Validado com dados
  reais**: martelo parado fica em `pp ~530–1840`; uma marretada inteira dá
  `pp ~12000–32000` (chega a 65535 batendo forte). 9000 cai no **meio do abismo** →
  marretada inteira dispara, segurar/parado não.
- **Cooldown: `COOLDOWN_MS = 700`** (700 ms mínimos entre cliques).
- **Macro**: gravação/replay **funcionam** (teste real gravou e reproduziu 414 eventos).
- **Debug sem Bluetooth**: o firmware **espelha o stream na porta USB do próprio Pico**.
  Basta plugar o cabo USB e apontar o `terminal.py` para a COM do Pico.

---

## 1. O que mudou nesta rodada (changelog detalhado)

### `main/gesture.cpp` (IA / marretada)
- **Gatilho trocado de modelo → energia.** Antes: disparava `MINE` quando o modelo
  dizia "porrada" com confiança ≥ 0.6. Agora: dispara quando `pp >= ENERGY_TH` (energia
  do swing), **por borda** (1 clique por movimento) **+ cooldown** de `COOLDOWN_MS`.
- Constantes novas no topo do arquivo: `ENERGY_TH = 9000`, `COOLDOWN_MS = 700`.
- O modelo do EI **continua rodando** (a inferência acontece), mas o rótulo dele **não
  é mais usado** para clicar — só aparece no log como referência (`m1`/`m0`).
- **Diagnóstico**: além de `AI:<label> <conf%> pp<pp>`, agora também emite
  `SW:ax<eixo> up<max> dn<min> pk@<i_max> tr@<i_min> m<0/1>` (a "assinatura" do swing
  no eixo dominante: máximo, mínimo, em que amostra ocorreram, e o voto do modelo).

### `main/main.c` (firmware / RTOS)
- **Espelho USB**: a `tx_task` agora escreve o **mesmo** stream que vai pra Bluetooth
  também na **serial USB do Pico** (protegido por `stdio_usb_connected()` para nunca
  travar quando não há host). Isso permite **debugar pelo cabo USB**, sem Bluetooth.
- `tx_msg_t.data` aumentado de 24 → **48 bytes** (cabem as linhas de diagnóstico sem
  cortar o `\n`).
- **Diagnóstico do macro**: em cada transição da FSM, manda uma linha ASCII —
  `MACRO:REC`, `MACRO:READY n=<N>`, `MACRO:PLAY n=<N>`, `MACRO:STOP`, `MACRO:DONE`
  (o `n` mostra quantos eventos foram gravados).
- **Self-test de LED no boot do `macro_task`**: acende os pinos do LED em sequência
  (~0,8 s cada) e manda `MACRO:SELFTEST`. *Obs.: o hardware atual não tem LED RGB de
  verdade; isso é só diagnóstico e é inofensivo — pode ignorar.*
- Include novo: `pico/stdio_usb.h`.

### `main/CMakeLists.txt` (build)
- `pico_enable_stdio_usb(pico_emb 1)` — habilita a serial USB (para o espelho).
- `pico_enable_stdio_uart(pico_emb 0)` — desliga o stdio na UART0 (não usamos GP0/GP1).

### `python/terminal.py` (interface PC)
- **Auto-conecta no Pico via USB**: `find_hc06_port()` agora procura **primeiro** o
  Pico pelo USB (VID `2E8A` da Raspberry Pi) e só depois o HC-06 por Bluetooth.
- **Afirma DTR ao conectar** (o stdio USB do Pico só "conecta" quando o host levanta o
  DTR — sem isso o Pico não espelha nada).
- **Novo checkbox "Injetar no jogo"** (desligado por padrão), **separado** do "Controle":
  - **"Controle"** = demultiplexa o stream (entende quadros de mouse + comandos ASCII)
    e mostra tudo no log.
  - **"Injetar no jogo" OFF** = **modo Monitor**: loga tudo **sem** mexer no
    teclado/mouse do desktop (ideal para testar sem o jogo, sem cliques fantasmas).
  - **"Injetar no jogo" ON** = move o cursor de verdade e aciona teclas/cliques.
- O log agora rotula as linhas de telemetria: `[ia]` (AI:), `[swing]` (SW:),
  `[macro]` (MACRO:), `[cmd]` (JUMP/MINE/PAUSE) e `[mouse]` (velocidade da câmera).

### Por que ignoramos o modelo de IA (evidência)
Capturado ao vivo pela telemetria (`pp` = pico-a-pico do eixo dominante):

| Situação              | `pp`                 | Modelo disse        |
|-----------------------|----------------------|---------------------|
| Parado / segurando    | 532, 560, 616, 1840  | **porrada 99%** ❌  |
| Swing forte           | 49352, 53284, 65007  | **idle 99%** ❌     |
| Swing forte           | 54311, 65535         | porrada 99% ✅      |

Ou seja, o rótulo do modelo é praticamente ruído. Já a **energia (`pp`) separa
limpo**: parado `< ~2000`, marretada `> ~12000`. Por isso o gatilho passou a ser a
energia. *(Se um dia o modelo for retreinado com dados melhores, dá para voltar a usar
o rótulo — o caminho continua no código, só não está no gatilho.)*

---

## 2. Compilar e gravar o firmware

### 2.1 Compilar
**Pelo VS Code (extensão Raspberry Pi Pico — recomendado):**
1. Abra a pasta do projeto no VS Code (a extensão configura sozinha; pode levar 1–2 min).
2. Clique em **Compile** (ícone da Pico na barra inferior → *Compile Project*), ou
   rode a task de build da extensão.
3. Saída: **`build/main/pico_emb.uf2`**.

**Pela linha de comando (alternativa):**
```powershell
cmake --build build
```
(Requer o `build/` já configurado pela extensão. O `.uf2` sai em `build/main/pico_emb.uf2`.)

> O `build/` é ignorado pelo Git de propósito — por isso o `.uf2` **não** vai pro
> repositório; quem testar precisa **compilar**.

### 2.2 Gravar (BOOTSEL)
1. Segure o botão **BOOTSEL** do Pico e plugue o USB (ou segure BOOTSEL e aperte RESET).
2. Aparece um disco **`RP2350`/`RPI-RP2`** no Windows.
3. Arraste **`build/main/pico_emb.uf2`** para esse disco. O Pico reinicia e roda.

> No boot o firmware espera ~3 s pela conexão USB e faz o self-test do LED (~2,4 s) —
> normal. A COM USB do Pico só aparece **depois** que ele sai do BOOTSEL e está rodando.

---

## 3. Conectar — duas opções

### 3.1 USB direto (recomendado para DEBUG / sem Minecraft)
1. Mantenha o **cabo USB do Pico** ligado (o mesmo do flash).
2. O Windows cria uma COM nova: **"Dispositivo Serial USB (COMxx)"**, hwid
   `USB VID:PID=2E8A:...`. (Veja em *Gerenciador de Dispositivos → Portas (COM e LPT)*.)
3. O `terminal.py` **auto-conecta nessa COM** (ele prioriza o VID 2E8A). No log aparece
   `[auto] Pico (USB) em COMxx -> conectando`.
4. Se não conectar sozinho: **Atualizar portas** → selecione a COM do Pico → **Conectar**.

### 3.2 Bluetooth (para jogar de verdade, sem fio)
No Windows, **parear ≠ conectar**: parear cria **duas** portas COM (outgoing/incoming),
mas a conexão SPP só acontece quando um programa **abre a porta outgoing**.
1. *Configurações → Bluetooth* → **parear** o módulo HC-06. Nome: `ENZO-BLUETOOTH`,
   PIN: **`1234`**. (O LED do HC-06 **piscando** = pareado mas não conectado.)
2. Rode o `terminal.py`. O auto-connect acha a porta **outgoing** do HC-06 (pelo MAC no
   hwid `BTHENUM`) e a abre → o LED do HC-06 fica **fixo** = **conectado**.
3. Se não achar: **Atualizar portas**, escolha a COM do HC-06 e **Conectar**. (Se houver
   mais de uma COM Bluetooth, a correta é a que deixa o LED do módulo **fixo**.)

> Dica para identificar as COMs (rode no PowerShell):
> ```powershell
> C:/Python313/python.exe -m serial.tools.list_ports -v
> ```
> Pico = `VID:PID=2E8A:...`; HC-06 = `BTHENUM\...` com MAC ≠ `000000000000`.

---

## 4. Abrir e usar o `terminal.py`

### 4.1 Dependências (uma vez)
```powershell
C:/Python313/python.exe -m pip install pyserial
```
(O `terminal.py` usa só `pyserial`; a injeção de teclado/mouse usa `ctypes`/Win32.)

### 4.2 Rodar
```powershell
C:/Python313/python.exe c:/Users/laskd/26a-emb-aps-2-vaul/python/terminal.py
```
- Ele **se eleva via UAC** (necessário para injetar input em jogos elevados).
- **Para DEBUG (sem jogo):** deixe **"Injetar no jogo" DESLIGADO** (modo Monitor).
- **Para JOGAR:** ligue **"Controle"** e **"Injetar no jogo"**.

### 4.3 Como ler o log
- `[ia] AI:porrada 99 pp23068` → predição do modelo (referência; **não** dispara o clique).
- `[swing] SW:ax1 up12236 dn-18908 pk@102 tr@115 m1` → assinatura do swing no eixo
  dominante. **`pp` = `up - dn`** é a energia que decide o disparo. `m1`/`m0` = voto do modelo.
- `[cmd] MINE  (monitor: nao injetado)` → a marretada **disparou** (em modo Monitor não
  clica; com "Injetar no jogo" ligado vira clique esquerdo de verdade).
- `[cmd] JUMP` / `[cmd] PAUSE` → botões (espaço / ESC).
- `[mouse] X=.. Y=..` → velocidade da câmera (joystick).
- `[macro] MACRO:REC` / `READY n=N` / `PLAY n=N` / `STOP` / `DONE` → estado do macro.

---

## 5. Calibrar a intensidade da marretada (FAÇA ISTO ANTES DE GRAVAR)

A sensibilidade é o limiar **`ENERGY_TH`** em `main/gesture.cpp`. Cada pessoa marreta com
força diferente — calibre para **quem vai testar**.

### 5.1 Procedimento (com o `terminal.py` em modo Monitor, "Injetar no jogo" OFF)
Observe a coluna **`pp`** nas linhas `[swing] SW:...` enquanto faz cada coisa:

1. **Fique parado / segure o martelo** ~10 s → anote o **maior `pp`** (esperado: `< ~2000`).
2. **Mexa como se estivesse jogando** (virar câmera, andar, reposicionar o martelo) **sem
   marretar** ~15 s → anote o **maior `pp`** (o "ruído de jogo").
3. **Dê a marretada inteira** (movimento completo, firme) 5–10x → anote o **menor `pp`**
   das marretadas.

| Medida                          | `pp` observado |
|---------------------------------|----------------|
| Parado (máx)                    | ______         |
| Jogando sem marretar (máx)      | ______         |
| Marretada inteira (mín)         | ______         |

### 5.2 Escolher o `ENERGY_TH`
Coloque o limiar **entre** o "ruído de jogo (máx)" e a "marretada (mín)", com folga —
e levemente puxado para baixo da marretada-mín para não perder marretadas:

```
ENERGY_TH  ≈  (ruído_de_jogo_máx  +  marretada_mín) / 2
```

Edite a linha em **`main/gesture.cpp`**:
```cpp
static const int ENERGY_TH = 9000;   // <-- ajuste aqui, depois recompile e regrave
```
Recompile (seção 2.1) e regrave (seção 2.2).

### 5.3 Recomendação de intensidade (quão forte marretar)
- **Marrete o movimento INTEIRO e com convicção** — subir e descer o martelo de forma
  decidida, **não** um tapinha/flick. Um movimento firme passa folgado dos `9000`
  (na prática deu `pp 12000–32000`).
- Segurar o martelo parado ou ajustes pequenos ficam abaixo de `~2000` → **não disparam**.
- **Os valores atuais (`ENERGY_TH = 9000`, `COOLDOWN_MS = 700`) deram certo** na análise
  dos dados reais: separam limpo "parado" de "marretada". Se o testador marretar mais
  fraco, **abaixe** o limiar (ex.: 7000); se disparar à toa mexendo o controle, **suba**
  (ex.: 12000).

---

## 6. Testar de verdade no Minecraft

### 6.1 Preparação
1. **Grave o firmware** (seção 2) e **calibre o `ENERGY_TH`** (seção 5).
2. Conecte por **Bluetooth** (seção 3.2) para jogar sem fio (USB também funciona, mas
   amarra o martelo ao PC).
3. Rode o `terminal.py` **como administrador** (ele se eleva sozinho) — necessário
   porque o Windows descarta injeção de input de processo não-elevado em jogos elevados.
4. Ligue **"Controle"** e **"Injetar no jogo"**.
5. Abra o Minecraft (Java, PC) e entre num mundo.

### 6.2 Mapeamento dos controles
| Ação física                         | Vai para o jogo            |
|-------------------------------------|----------------------------|
| **Marretada inteira** (martelo)     | **Clique esquerdo** = minerar/quebrar bloco |
| **Joystick** (analógico)            | **Câmera** (mouse, modelo de velocidade)    |
| Botão do joystick (apertar pra baixo)| **Espaço** (pular)        |
| Botão de pause                      | **ESC**                    |

- Ajuste a sensibilidade da câmera no slider **"Sens."** em tempo real até ficar bom.
- Se aparecer `[cmd] ... -> BLOQUEADO pelo Windows`, o `terminal.py` **não está como
  administrador** (feche e rode de novo aceitando o UAC).

### 6.3 Testando a marretada no jogo
- Olhe para um bloco e **dê a marretada inteira**. O clique esquerdo segura/quebra.
- A inferência roda em **janelas de ~2 s** — pode haver pequeno atraso entre o gesto e o
  clique. É esperado nessa arquitetura.
- Se não estiver quebrando como deveria, volte à **calibração (seção 5)** com o jogo
  fechado (modo Monitor) e ajuste o `ENERGY_TH`.

### 6.4 Usando o macro (gravar e repetir uma sequência)
O macro grava **tudo** que você faz (câmera, pulos, marretadas, pause) e repete com o
mesmo tempo. Controlado por **1 botão** (GPIO 12). Ciclo de estados:

```
VAZIO --click--> GRAVANDO --click--> PRONTO --click--> TOCANDO --(fim)--> VAZIO
                                                         \--click--> PRONTO (interrompe)
```
1. **Click 1** → começa a **gravar** (`MACRO:REC` no log). Faça a sequência que quer
   repetir (ex.: girar a câmera + marretar várias vezes).
2. **Click 2** → **para de gravar** e fica **pronto** (`MACRO:READY n=<N>`; `N` = nº de
   eventos gravados; se `N=0`, nada foi capturado).
3. **Click 3** → **reproduz** a sequência (`MACRO:PLAY n=<N>`). O input ao vivo fica
   suspenso durante a reprodução.
4. Ao terminar sozinho → `MACRO:DONE` e volta a **VAZIO**. Um **click durante a reprodução**
   **interrompe** e volta a **PRONTO** (`MACRO:STOP`, mantém a gravação).

- Capacidade: ~20 s de gravação (4000 eventos). Tudo em RAM → **zera ao reiniciar** o Pico.
- Verificação **sem Minecraft**: em modo Monitor, ao reproduzir você vê no log o "burst"
  de `[mouse]`/`[cmd]` repetindo o que foi gravado (foi assim que validamos: 414 eventos).

---

## 7. Problemas comuns (troubleshooting)

| Sintoma | Causa / solução |
|---|---|
| `ModuleNotFoundError: No module named 'serial'` | Falta o pyserial: `C:/Python313/python.exe -m pip install pyserial`. |
| `terminal.py` fica em loop *conectando → conexão perdida* | Estava pegando uma COM Bluetooth morta. Agora ele prioriza o Pico USB; ou clique **Desconectar**, **Atualizar portas**, escolha a COM certa e **Conectar**. |
| Não sai **nada** no log | Confirme que conectou na **COM do Pico (VID 2E8A)** e que o firmware novo está gravado. A cada ~2 s deve sair `[ia] AI:...` e `[swing] SW:...`. |
| Marretada **não dispara** | `ENERGY_TH` alto demais → **abaixe** (seção 5). Veja o `pp` da sua marretada no log. |
| Dispara **à toa** mexendo o controle | `ENERGY_TH` baixo demais → **suba** (seção 5). |
| `[cmd] ... BLOQUEADO pelo Windows` | Rode o `terminal.py` **como administrador** (aceite o UAC). |
| Câmera gira sozinha / não para | Normal só se o joystick estiver desregulado; o firmware envia 0 ao centrar. |

---

## 8. Valores e referências rápidas

- **Gatilho da marretada** (`main/gesture.cpp`): `ENERGY_TH = 9000`, `COOLDOWN_MS = 700`.
- **Janela de inferência**: 200 amostras @ 100 Hz = ~2 s (acelerômetro RAW do MPU6050).
- **Bluetooth**: nome `ENZO-BLUETOOTH`, PIN `1234`.
- **USB do Pico**: `VID:PID = 2E8A:...` (Raspberry Pi).
- **Comandos no stream**: `MINE` (clique esq.), `JUMP` (espaço), `PAUSE` (ESC); câmera =
  quadros binários `0xFF axis valor16`.
- **Telemetria de debug**: `AI:` (modelo), `SW:` (energia/assinatura), `MACRO:` (estado).
- **Pinos**: macro botão GP12; MPU6050 i2c0 (GP8/GP9); OLED i2c1 (GP2/GP3); joystick ADC
  GP26/GP27, SW GP15; pause GP14; pin-BT GP10.
