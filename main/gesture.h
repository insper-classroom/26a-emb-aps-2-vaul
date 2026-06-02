#ifndef GESTURE_H_
#define GESTURE_H_

// Ponte C/C++ entre o firmware (main.c, em C) e a inferencia do Edge Impulse
// (gesture.cpp, em C++). O bloco extern "C" garante linkagem C nos dois lados:
// o main.c ve declaracoes C normais; o gesture.cpp desliga o name-mangling do
// C++ nesses simbolos para o linker casar.

#ifdef __cplusplus
extern "C" {
#endif

// Task FreeRTOS que le o MPU6050 (i2c0), roda o classificador e dispara "MINE".
// Definida em gesture.cpp.
void gesture_task(void *p);

// Envia um comando ASCII pela Bluetooth (wrapper do send_command estatico).
// Definida em main.c; usada pela ponte para enviar "MINE\n".
void controller_send_command(const char *s);

#ifdef __cplusplus
}
#endif

#endif // GESTURE_H_
