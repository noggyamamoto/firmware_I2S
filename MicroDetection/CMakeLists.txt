# ============================================================================
# Projeto ESP-IDF do firmware MicroDetection.
#
# Estrutura:
#   drivers/   controle direto de hardware (GPIO/LED embutido, PWM, I2S, UART, NVS, Wi-Fi)
#   hal/       camada de abstração com funções simples e reutilizáveis
#   app/       lógica da aplicação (fluxo de controle, protocolo, DSP, interface serial)
#   include/   cabeçalhos necessários
#
# O componente principal fica em app/ (PlatformIO: src_dir = app).
# Compilação direta com o ESP-IDF:  idf.py set-target esp32 build
# ============================================================================
cmake_minimum_required(VERSION 3.16.0)

if(NOT "${EXTRA_COMPONENT_DIRS}" MATCHES "/app")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/app")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MicroDetection)
