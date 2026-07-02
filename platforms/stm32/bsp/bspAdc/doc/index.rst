..
   *******************************************************************************
   Copyright (c) 2026 An Dao

   This program and the accompanying materials are made available under the
   terms of the Apache License Version 2.0 which is available at
   https://www.apache.org/licenses/LICENSE-2.0

   SPDX-License-Identifier: Apache-2.0
   *******************************************************************************

bspAdc Driver
=============

Overview
--------

The ``bspAdc`` module provides a single-conversion, polling-mode ADC driver
for STM32 targets. The driver is the ``bios::Adc`` class; an instance is
described by an ``AdcConfig`` struct (ADC peripheral, resolution, sampling
time code).

``init()`` enables the ADC clock (system clock as kernel clock on STM32G4),
runs the hardware calibration where available, connects the internal
temperature sensor and VREFINT paths, and enables the ADC.
``readChannel()`` performs one software-triggered conversion of the given
channel. ``readTemperature()`` and ``readVrefint()`` convert the internal
channels (temperature: ADC1 IN16 on G4, IN18 on F413; VREFINT: IN18 on G4,
IN17 on F413).

All operations return ``bsp::BspReturnCode``; every hardware wait is bounded
and reports ``BSP_ERROR`` instead of blocking indefinitely if the ADC does
not respond.

Configuration notes
-------------------

The internal temperature sensor requires a minimum sampling time (see the
device datasheet, typically at least 5 us); choose the ``samplingTime``
code accordingly when reading the internal channels.
