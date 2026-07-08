..
   *******************************************************************************
   Copyright (c) 2026 An Dao

   This program and the accompanying materials are made available under the
   terms of the Apache License Version 2.0 which is available at
   https://www.apache.org/licenses/LICENSE-2.0

   SPDX-License-Identifier: Apache-2.0
   *******************************************************************************

.. _stm32_overview:

STM32
=====

Overview
--------

- Eclipse OpenBSW supports STM32 targets from the STMicroelectronics STM32F4 and
  STM32G4 families.
- The platform provides the board-support layer for these devices: system
  clock/PLL configuration, GPIO, UART, timers, interrupt control and CAN
  (bxCAN on STM32F4, FDCAN on STM32G4).
- STM32 support is being integrated into Eclipse OpenBSW incrementally, module
  by module.

Supported targets
-----------------

.. csv-table::
   :header: "Board", "MCU", "Core"
   :widths: 20, 20, 20

   "NUCLEO-F413ZH", "STM32F413ZH", "Arm Cortex-M4"
   "NUCLEO-G474RE", "STM32G474RE", "Arm Cortex-M4"

The target MCU is selected at configure time through the ``STM32_CHIP`` CMake
variable (``STM32F413ZH`` or ``STM32G474RE``).

Build environment
-----------------

For general instructions on building Eclipse OpenBSW see :ref:`learning_setup`.

Reference Links
---------------

1. **Development boards**:
    - `NUCLEO-F413ZH <https://www.st.com/en/evaluation-tools/nucleo-f413zh.html>`_
    - `NUCLEO-G474RE <https://www.st.com/en/evaluation-tools/nucleo-g474re.html>`_

2. **Microcontrollers**:
    - `STM32F413ZH <https://www.st.com/en/microcontrollers-microprocessors/stm32f413zh.html>`_
    - `STM32G474RE <https://www.st.com/en/microcontrollers-microprocessors/stm32g474re.html>`_

3. **Reference Manuals**:
    - STM32F413 - Reference Manual RM0430
    - STM32G4 series - Reference Manual RM0440
