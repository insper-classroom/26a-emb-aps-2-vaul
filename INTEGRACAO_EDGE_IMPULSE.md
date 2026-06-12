# Integração Edge Impulse (MPU6050 → "MINE") — LEIA ANTES DE SUBIR UM MODELO NOVO

> **Para quem vai treinar/atualizar a rede no Edge Impulse e re-exportar a lib.**
> Nós passamos **horas** depurando um bug em que, só de linkar o EI, a Pico **morria no
> boot** (OLED apagado, nada funcionava, nem o LED). A correção é pequena e **mora em
> arquivos do `main/`**, não no `ei-model/`. Se você sobrescrever as coisas erradas ao
> atualizar o modelo, **o bug volta**. Este documento existe para isso não acontecer.

---

## 1. TL;DR — as regras de ouro

1. **Ao atualizar o modelo, substitua APENAS o conteúdo de `ei-model/`** (`edge-impulse-sdk/`,
   `model-parameters/`, `tflite-model/`). **NÃO** toque em `main/` nem no `CMakeLists.txt` da raiz.
2. **NUNCA remova o override de alocador em `main/gesture.cpp`** (`ei_malloc` / `ei_calloc` /
   `ei_free` → `pvPortMalloc` / `vPortFree`). **É essa a correção do bug de boot.** Sem ela a
   placa morre antes do `main()`.
3. **`main/gesture.cpp` é o ÚNICO arquivo que faz `#include "...ei_run_classifier.h"`.** Não
   inclua esse header em outro `.cpp` (geraria "multiple definition of run_classifier").
4. **MPU6050 fica no `i2c0`, GP8 = SDA, GP9 = SCL.** (O exemplo de referência usava GP4/GP5,
   que aqui são a UART do Bluetooth HC-06. Não copie GP4/GP5.)
5. Depois de atualizar, **ajuste a label do gesto** em `main/gesture.cpp` (ver §6) e **valide
   o build** (ver §7).

Se a placa voltar a "morrer no boot" (OLED nunca acende), a causa é quase certamente uma destas
regras quebrada — volte aqui.

---

## 2. Sintoma do bug (para reconhecer se ele voltar)

- Com o EI linkado, a Pico **não dá sinal de vida**: OLED nunca liga, joystick não mexe o
  cursor, **nenhum LED**. Só a conexão Bluetooth aparece "conectada" no PC (isso engana: o
  **HC-06 é um módulo autônomo** e mantém o link mesmo com a Pico travada).
- **Sem o EI** (ou com a IA desligada), tudo funciona normalmente.
- O firmware morre **antes do `main()`** (nem o pisca de boot acontece).

---

## 3. Causa raiz (o que realmente acontecia)

O Edge Impulse usa C++ pesado (`std::vector`, `std::function`, features espectrais com FFT).
A lib tem um **construtor estático** (em `ei::spectral`) que **chama `ei_malloc` ANTES do
`main()`** (durante `__libc_init_array`, a inicialização de objetos globais C++).

Por padrão (sem `FREERTOS_ENABLED` definido), `ei_malloc` cai no **`malloc` da libc**. No
Pico **com FreeRTOS**, esse `malloc` é "embrulhado" (`__wrap_malloc`) e passa por um **mutex**
que, para saber quem é o dono do lock, lê a **task atual** (`pxCurrentTCB`,
`xTaskGetCurrentTaskHandle`).

**Antes do escalonador iniciar, não existe task atual: `pxCurrentTCB` é NULL.** Resultado:
o `malloc` pré-`main()` bate nesse caminho e **trava/faz fault antes do `main()`**.

O firmware **baseline funcionava** porque ele **nunca chamava `malloc` antes do `main()`** — só
o EI introduziu uma alocação em tempo de construção estática.

> Detalhe que confundia: o EI só entra no binário porque `gesture.cpp` referencia
> `run_classifier`. Mas o problema **não** é "linkar o EI" em si — é **alocar memória pela
> libc antes do escalonador**.

---

## 4. A correção (o que foi feito e ONDE)

**Arquivo: `main/gesture.cpp`** — sobrescrevemos as funções `weak` de alocação do EI para
usarem o heap do **FreeRTOS** (`pvPortMalloc`/`vPortFree`), que é **seguro antes do `main()`**
(o próprio `xTaskCreate` no `main()` já usa `pvPortMalloc` antes do escalonador e funciona) **e**
thread-safe em runtime:

```cpp
// NÃO REMOVER. Esta é a correção do crash de boot do Edge Impulse.
// (definições com linkagem C++, iguais ao header do EI; "fortes" vencem as "weak" do porting)
void *ei_malloc(size_t size)                 { return pvPortMalloc(size); }
void *ei_calloc(size_t nitems, size_t size)  { void *p = pvPortMalloc(nitems*size);
                                               if (p) memset(p, 0, nitems*size); return p; }
void  ei_free(void *ptr)                     { vPortFree(ptr); }
```

Por que assim (e não outras abordagens):
- **NÃO** definimos `FREERTOS_ENABLED` no CMake: isso faria o EI compilar seu próprio
  `pvPortCalloc`, colidindo com o do `heap_4.c`. O override em `gesture.cpp` evita o define.
- **NÃO** usamos `__real_malloc` (libc cru, sem mutex): não é thread-safe e a inferência aloca
  bastante → corromperia o heap concorrendo com outras tasks.

**Arquivo: `main/CMakeLists.txt`** — pontos que devem permanecer:
- `gesture.cpp` está no `add_executable(pico_emb ...)`.
- O EI é coletado por GLOB (`RECURSIVE_FIND_FILE` + `target_sources(... ${EI_SOURCES})`) e os
  diretórios `ei-model/...` estão nos `target_include_directories`.
- `target_compile_options(pico_emb PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)` — higiene padrão de TFLite/EI em microcontrolador
  (menor binário, sem maquinaria de exceções). Sozinho **não** consertava o boot, mas é correto.

**`CMakeLists.txt` da raiz** já está com `project(pico_emb C CXX ASM)` + C11/C++17 (mistura
C/C++). O `main.c` continua em C; `gesture.cpp` é a ponte C++ que expõe `gesture_task()` com
`extern "C"` (ver `main/gesture.h`).

---

## 5. Becos sem saída (o que NÃO era o problema — não repita)

Para você não perder tempo nas mesmas pistas falsas:
- **Não** era falta de RAM (`.bss` ~196 KB de 520 KB).
- **Não** eram os construtores estáticos "fazendo algo perigoso" — o do `tflite_*` é trivial.
- **Não** era exceções de C++ — `-fno-exceptions` **não** mudou o binário nem consertou (o
  runtime de exceções entra por referência da libstdc++, não pelos `throw`).
- **Não** era o `.uf2` corrompido nem o processo de gravação (BOOTSEL) — o baseline gravava e
  rodava perfeito pela mesma toolchain.
- **A única diferença** entre "funciona" e "morto" era **alocar pela libc antes do `main()`**.

---

## 6. Ajustes ao trocar de modelo (estado atual: Impulse #2, "porrada")

O modelo atual (`mdlask-project-1` Impulse #2, integrado em jun/2026) tem as labels
(em `ei-model/model-parameters/model_variables.h`):

```c
{ "idle", "porrada" }   // índices 0, 1 — 100 Hz, 200 amostras × 3 eixos (600 floats)
```

A `main/gesture.cpp` dispara o clique (`MINE`) quando o gesto vencedor é o de índice
`LABEL_PORRADA` (= 1) **e** a confiança ≥ `PORRADA_CONFIDENCE` (= 0.6). Ao subir um novo modelo:

1. **Veja as labels novas** em `model_variables.h` (o array `ei_classifier_inferencing_categories_*`)
   e descubra o **índice** do gesto que deve disparar o clique (no modelo atual,
   `"porrada"` é o índice **1**).
2. **Atualize em `main/gesture.cpp`** (renomeie para clareza, conforme o label):
   ```c
   static const size_t LABEL_PORRADA = 1;          // índice do gesto no modelo
   static const float  PORRADA_CONFIDENCE = 0.6f;  // ajuste a sensibilidade aqui
   ```
   e a checagem do disparo (`best == LABEL_PORRADA && ...value >= PORRADA_CONFIDENCE`).
3. **Sensibilidade:** ajuste `PORRADA_CONFIDENCE` (menor = dispara mais fácil, mais falsos
   positivos; maior = mais exigente). O disparo é **por borda** (1 clique por gesto; só rearma
   quando sai do estado) — isso é intencional para "quebrar bloco a bloco".
4. **Taxa de amostragem:** o código amostra a cada `pdMS_TO_TICKS(10)` (= 100 Hz) para casar
   com o `EI_CLASSIFIER_FREQUENCY = 100 Hz` do modelo atual. **Se você treinar em outra
   frequência, ajuste `SAMPLE_PERIOD_TICKS`** em `gesture.cpp` para `1000 / nova_frequencia` ms.
5. **Escala dos dados:** o firmware alimenta o acelerômetro como **int16 cru** (`(float)accel[x]`),
   igual ao exemplo de referência. Treine no Edge Impulse com o **mesmo dado cru** (ex.: data
   forwarder enviando os valores raw do MPU). Se treinar em outra escala (g, m/s²), a inferência
   não vai casar.
6. **Orientação do sensor:** monte o MPU na **mesma orientação** usada para coletar os dados de
   treino; senão os eixos trocam e o gesto "down" pode cair em outro eixo.

---

## 7. Como validar depois de atualizar (sem perder horas)

**Build (no PC):** compile normalmente. Se aparecer:
- `undefined reference to run_classifier` → alguém tirou o `#include ".../ei_run_classifier.h"`
  de `gesture.cpp` (ele precisa ser o único TU que o inclui).
- `multiple definition of run_classifier` → algum outro `.cpp` passou a incluir esse header.
- erro de `ei_malloc`/`ei_calloc` → conferir que as definições em `gesture.cpp` estão **sem**
  `extern "C"` (a linkagem é C++, igual ao header do EI).

**Conferência rápida no binário** (opcional, mas mata a dúvida). Com a toolchain ARM:
```
arm-none-eabi-objdump -d build/pico_emb.elf --disassemble=_Z9ei_mallocj
```
Tem que mostrar `b.w <pvPortMalloc>`. Se mostrar `malloc`/`mutex_enter_blocking`, o override
**não** pegou → o bug de boot vai voltar.

**Na placa (BOOTSEL + arrastar o `.uf2`):**
1. Ao energizar, o LED de status deve **dar sinais de boot** (ver os diagnósticos em §8). Se o
   LED **não faz nada** e o OLED não liga → bug de boot de volta (revise §1).
2. OLED mostra o PIN, joystick move o cursor.
3. No `terminal.py` (modo Controle), aparecem linhas de debug da IA; fazer o gesto treinado
   dispara o clique (`MINE` → bloco quebra).

---

## 8. Diagnósticos temporários (podem ser removidos na versão final)

Para depurar sem debugger/COM, deixamos instrumentação que usa o **LED do GP13** e a Bluetooth.
**Não são necessários para o funcionamento** — remover quando quiser a versão "limpa":

- `main/main.c`: pisca de boot (3 piscadas rápidas no início do `main()` + 1 piscada sólida de
  1s) e os handlers `isr_hardfault` / `vApplicationStackOverflowHook` (piscam padrões no GP13).
- `main/FreeRTOSConfig.h`: `configCHECK_FOR_STACK_OVERFLOW = 2` (era 0).
- `main/gesture.cpp`: o envio da linha de debug `"AI:<label> <conf> pp<pico-a-pico>"` pela
  Bluetooth (o `pp` = quanto o acelerômetro variou na janela — útil para distinguir "MPU sem
  dado" de "modelo não detecta").
- `python/terminal.py`: o `_handle_command` mostra comandos desconhecidos como `[ia] ...` (para
  exibir as linhas `AI:`); na versão final ele pode voltar a ignorá-los.

> Dica que salvou o debug: o `pp` (pico-a-pico do acelerômetro) distingue na hora **infra**
> (fio solto → `pp` ~0 mesmo mexendo) de **modelo** (`pp` grande mexendo, mas classifica errado).

---

## 9. Mapa de pinos (referência rápida)

| Periférico | Pinos | Barramento |
|---|---|---|
| MPU6050 (IA) | GP8 = SDA, GP9 = SCL | **i2c0** |
| OLED SSD1306 | GP2 = SDA, GP3 = SCL | i2c1 |
| HC-06 Bluetooth | GP4 = RX, GP5 = TX, GP6 = EN, GP7 = STATE | UART1 |
| Botão PIN | GP10 | — |
| Botão macro / LED macro | GP12 / GP13 | — |
| Botão Pause | GP14 | — |
| Joystick | GP26 = VRX, GP27 = VRY, GP15 = SW | ADC |
