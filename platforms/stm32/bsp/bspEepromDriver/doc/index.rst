..
   *******************************************************************************
   Copyright (c) 2026 An Dao

   This program and the accompanying materials are made available under the
   terms of the Apache License Version 2.0 which is available at
   https://www.apache.org/licenses/LICENSE-2.0

   SPDX-License-Identifier: Apache-2.0
   *******************************************************************************

bspEepromDriver
===============

Overview
--------

The ``bspEepromDriver`` module implements ``IEepromDriver`` on top of the
internal flash using two flash pages in a ping-pong scheme: the active page
holds the current data behind a magic marker, and on every write the full
updated page image is programmed into the erased inactive page before the
roles swap and the old page is erased.

The driver is ``eeprom::FlashEepromDriver``; its ``Config`` supplies the two
page base addresses and the page size. Page addresses must be 8-byte aligned
and the page size a multiple of 8 (STM32G4 flash is programmed in aligned
double words). All flash status error flags are checked and cleared around
every erase and program operation, and all waits are bounded.

Limitations
-----------

- The scheme targets STM32G4 (2KB pages, default dual-bank layout). The
  smallest STM32F413 erase unit is a 16KB sector, which exceeds the page
  buffer; writes are rejected on that target.
- Wear leveling is page-level only: every write erases and reprograms one
  page. Callers that write frequently should batch updates.
- If power is lost after the new page is programmed but before the old page
  is erased, both pages carry a valid marker and ``init()`` selects page 0.
  A future revision can add a sequence counter to disambiguate.
