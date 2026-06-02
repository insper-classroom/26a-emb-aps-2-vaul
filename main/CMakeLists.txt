if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping FreeRTOS examples as FREERTOS_KERNEL_PATH not defined")
    return()
endif()

include(FreeRTOS_Kernel_import.cmake)

# --- Edge Impulse (rede neural de deteccao de movimento) ---
# Biblioteca exportada do Edge Impulse, copiada para ei-model/ na raiz do projeto.
# O macro RECURSIVE_FIND_FILE vem dentro de edge-impulse-sdk/cmake/utils.cmake.
set(MODEL_FOLDER  ${CMAKE_SOURCE_DIR}/ei-model)
set(EI_SDK_FOLDER ${MODEL_FOLDER}/edge-impulse-sdk)
include(${EI_SDK_FOLDER}/cmake/utils.cmake)

# Example running FreeRTOS on 1 core
add_executable(pico_emb
    main.c
    hc06.c
    ssd1306.c
    gesture.cpp   # ponte C++ p/ a IA do Edge Impulse (deteccao de gesto -> MINE)
    )

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    # Edge Impulse: includes das 3 pastas (apenas o necessario da lib pura)
    ${MODEL_FOLDER}
    ${MODEL_FOLDER}/tflite-model
    ${MODEL_FOLDER}/model-parameters
    ${EI_SDK_FOLDER}
    ${EI_SDK_FOLDER}/third_party/ruy
    ${EI_SDK_FOLDER}/third_party/gemmlowp
    ${EI_SDK_FOLDER}/third_party/flatbuffers/include
    ${EI_SDK_FOLDER}/third_party
    ${EI_SDK_FOLDER}/tensorflow
    ${EI_SDK_FOLDER}/dsp
    ${EI_SDK_FOLDER}/classifier
    ${EI_SDK_FOLDER}/anomaly
    ${EI_SDK_FOLDER}/CMSIS/NN/Include
    ${EI_SDK_FOLDER}/CMSIS/DSP/PrivateInclude
    ${EI_SDK_FOLDER}/CMSIS/DSP/Include
    ${EI_SDK_FOLDER}/CMSIS/Core/Include
    ${EI_SDK_FOLDER}/porting
    )

# Linking to FreeRTOS-Kernel-Heap4 means we use a dynamic heap for allocations
target_link_libraries(pico_emb PRIVATE
    pico_async_context_freertos
    FreeRTOS-Kernel-Heap4
    hardware_adc
    hardware_i2c
    hardware_pwm
    pico_stdlib
    )
# Set the nunber of cores to 1.
# This defaults to 2 in FreeRTOSConfig_examples_common.h if not defined in here
target_compile_definitions(pico_emb PRIVATE
    configNUMBER_OF_CORES=1
    # Edge Impulse: defines da lib.
    # NAO definir FREERTOS_ENABLED: o porting do EI usa #ifdef (testa presenca,
    # nao valor), entao defini-lo faz o EI compilar seu proprio pvPortCalloc, que
    # colide com o do FreeRTOS novo (heap_4.c) ja presente neste projeto. Sem o
    # define, o EI usa malloc/calloc/free da libc (default e compativel).
    EIDSP_QUANTIZE_FILTERBANK=0
    ARM_MATH_LOOPUNROLL
    )
# Edge Impulse / TFLite Micro em microcontrolador: compilar o C++ SEM exceptions
# nem RTTI (padrao do EI). Sem isso, o runtime de excecoes do libstdc++ entra no
# binario com construtores estaticos que travam no boot (antes do main()).
target_compile_options(pico_emb PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
if(PICO_CYW43_SUPPORTED)
    # For led support on pico_w
    target_link_libraries(pico_emb PRIVATE
        pico_cyw43_arch_none
        )
endif()
# --- Edge Impulse: coleta recursiva dos fontes da lib e do modelo ---
# Os arquivos de porting/ do EI sao auto-guardados por macro, entao globar todo o
# edge-impulse-sdk e seguro (so a porting do alvo emite codigo). Nao re-adicionar
# porting/raspberry separadamente para evitar simbolo duplicado.
RECURSIVE_FIND_FILE(EI_CPP   "${EI_SDK_FOLDER}" "*.cpp")
RECURSIVE_FIND_FILE(EI_CC    "${EI_SDK_FOLDER}" "*.cc")
RECURSIVE_FIND_FILE(EI_C     "${EI_SDK_FOLDER}" "*.c")
RECURSIVE_FIND_FILE(EI_S     "${EI_SDK_FOLDER}" "*.s")
RECURSIVE_FIND_FILE(EI_MODEL "${MODEL_FOLDER}/tflite-model" "*.cpp")
list(APPEND EI_SOURCES ${EI_CPP} ${EI_CC} ${EI_C} ${EI_S} ${EI_MODEL})
target_sources(pico_emb PRIVATE ${EI_SOURCES})

pico_add_extra_outputs(pico_emb)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
