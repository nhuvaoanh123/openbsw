/********************************************************************************
 * Copyright (c) 2026 An Dao
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

#include <eeprom/FlashEepromDriver.h>

#include <mcu/mcu.h>

#include <cstring>

namespace
{
uint32_t const DATA_OFFSET = 4U; // Magic word occupies the first 4 bytes

#if defined(STM32G474xx)
uint32_t const FLASH_ERROR_FLAGS = FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR
                                   | FLASH_SR_PGAERR | FLASH_SR_SIZERR | FLASH_SR_PGSERR
                                   | FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR;
#elif defined(STM32F413xx)
uint32_t const FLASH_ERROR_FLAGS = FLASH_SR_SOP | FLASH_SR_WRPERR | FLASH_SR_PGAERR
                                   | FLASH_SR_PGPERR | FLASH_SR_PGSERR | FLASH_SR_RDERR;

// STM32F413 flash sector layout (RM0430, single bank): sectors 0-3 are 16KB,
// sector 4 is 64KB, sectors 5-15 are 128KB (1.5MB total). pageAddr must be
// the base address of the sector to erase.
bool sectorFromPageAddress(uintptr_t const pageAddr, uint32_t& sector)
{
    if (pageAddr < FLASH_BASE)
    {
        return false;
    }
    uintptr_t const offset = pageAddr - FLASH_BASE;
    if (offset < 0x10000U)
    {
        if ((offset % 0x4000U) != 0U)
        {
            return false;
        }
        sector = static_cast<uint32_t>(offset / 0x4000U);
        return true;
    }
    if (offset == 0x10000U)
    {
        sector = 4U;
        return true;
    }
    if ((offset >= 0x20000U) && (offset < 0x180000U))
    {
        if (((offset - 0x20000U) % 0x20000U) != 0U)
        {
            return false;
        }
        sector = 5U + static_cast<uint32_t>((offset - 0x20000U) / 0x20000U);
        return true;
    }
    return false;
}
#endif
} // namespace

namespace eeprom
{

FlashEepromDriver::FlashEepromDriver(Config const& config) : fConfig(config), fActivePage(0U) {}

bsp::BspReturnCode FlashEepromDriver::init()
{
    // Both pages must be 8-byte aligned (G4 double-word programming) and the
    // page size must cover the magic word and be a multiple of 8.
    if (((fConfig.page0Address & 0x7U) != 0U) || ((fConfig.page1Address & 0x7U) != 0U)
        || ((fConfig.pageSize & 0x7U) != 0U) || (fConfig.pageSize <= DATA_OFFSET))
    {
        return bsp::BSP_ERROR;
    }

    if (isPageValid(fConfig.page0Address))
    {
        fActivePage = 0U;
    }
    else if (isPageValid(fConfig.page1Address))
    {
        fActivePage = 1U;
    }
    else
    {
        // Neither page valid - format page 0
        if (erasePage(fConfig.page0Address) != bsp::BSP_OK)
        {
            return bsp::BSP_ERROR;
        }

        if (unlockFlash() != bsp::BSP_OK)
        {
            return bsp::BSP_ERROR;
        }
        uint32_t const magic = MAGIC;
        if (programPage(fConfig.page0Address, reinterpret_cast<uint8_t const*>(&magic), 4U)
            != bsp::BSP_OK)
        {
            lockFlash();
            return bsp::BSP_ERROR;
        }
        lockFlash();
        fActivePage = 0U;
    }

    return bsp::BSP_OK;
}

bsp::BspReturnCode FlashEepromDriver::read(uint32_t address, uint8_t* buffer, uint32_t length)
{
    uint32_t const dataSize = fConfig.pageSize - DATA_OFFSET;
    if ((length == 0U) || (address >= dataSize) || (length > (dataSize - address)))
    {
        return bsp::BSP_ERROR;
    }

    uintptr_t const srcAddr = activePageAddress() + DATA_OFFSET + address;
    std::memcpy(buffer, reinterpret_cast<void const*>(srcAddr), length);
    return bsp::BSP_OK;
}

bsp::BspReturnCode
FlashEepromDriver::write(uint32_t address, uint8_t const* buffer, uint32_t length)
{
    uint32_t const dataSize = fConfig.pageSize - DATA_OFFSET;
    if ((length == 0U) || (address >= dataSize) || (length > (dataSize - address)))
    {
        return bsp::BSP_ERROR;
    }

    // Static buffer for the full page image (magic word + data) - avoids
    // stack allocation in the calling task. Sized for G474RE (2KB pages).
    // F413ZH has no flash granularity suitable for this ping-pong scheme
    // (its smallest sectors are 16KB); writes there are rejected here.
    static uint8_t pageBuf[2048];
    if (fConfig.pageSize > sizeof(pageBuf))
    {
        return bsp::BSP_ERROR;
    }

    // Assemble the new page image: magic word, current data, caller's update.
    uint32_t const magic = MAGIC;
    std::memcpy(&pageBuf[0], &magic, sizeof(magic));
    std::memcpy(
        &pageBuf[DATA_OFFSET],
        reinterpret_cast<void const*>(activePageAddress() + DATA_OFFSET),
        dataSize);
    std::memcpy(&pageBuf[DATA_OFFSET + address], buffer, length);

    uintptr_t const inactiveAddr = inactivePageAddress();
    if (erasePage(inactiveAddr) != bsp::BSP_OK)
    {
        return bsp::BSP_ERROR;
    }

    if (unlockFlash() != bsp::BSP_OK)
    {
        return bsp::BSP_ERROR;
    }

    // Program the whole page in one pass so every double word (including the
    // one shared by the magic word and the first data bytes) is written
    // exactly once, at an aligned address.
    if (programPage(inactiveAddr, pageBuf, fConfig.pageSize) != bsp::BSP_OK)
    {
        lockFlash();
        return bsp::BSP_ERROR;
    }

    lockFlash();

    uintptr_t const oldActiveAddr = activePageAddress();
    fActivePage                   = (fActivePage == 0U) ? 1U : 0U;

    // The data is committed at this point; a failed erase of the old page
    // is still reported so the caller knows the flash needs attention.
    return erasePage(oldActiveAddr);
}

bsp::BspReturnCode FlashEepromDriver::erase()
{
    if (erasePage(fConfig.page0Address) != bsp::BSP_OK)
    {
        return bsp::BSP_ERROR;
    }
    if (erasePage(fConfig.page1Address) != bsp::BSP_OK)
    {
        return bsp::BSP_ERROR;
    }
    return init();
}

uintptr_t FlashEepromDriver::activePageAddress() const
{
    return (fActivePage == 0U) ? fConfig.page0Address : fConfig.page1Address;
}

uintptr_t FlashEepromDriver::inactivePageAddress() const
{
    return (fActivePage == 0U) ? fConfig.page1Address : fConfig.page0Address;
}

bool FlashEepromDriver::isPageValid(uintptr_t pageAddr) const
{
    uint32_t const magic = *reinterpret_cast<uint32_t const volatile*>(pageAddr);
    return magic == MAGIC;
}

bsp::BspReturnCode FlashEepromDriver::unlockFlash()
{
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U)
    {
        FLASH->KEYR = 0x45670123U;
        FLASH->KEYR = 0xCDEF89ABU;
    }
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U)
    {
        return bsp::BSP_ERROR;
    }
    return bsp::BSP_OK;
}

void FlashEepromDriver::lockFlash() { FLASH->CR |= FLASH_CR_LOCK; }

void FlashEepromDriver::clearFlashErrors()
{
    // Error flags are write-1-to-clear; stale flags block new operations.
    FLASH->SR = FLASH_ERROR_FLAGS;
}

bsp::BspReturnCode FlashEepromDriver::waitForFlash()
{
    uint32_t timeout = 0xFFFFFFU;
    while ((FLASH->SR & FLASH_SR_BSY) != 0U)
    {
        if (--timeout == 0U)
        {
            return bsp::BSP_ERROR;
        }
    }
    if ((FLASH->SR & FLASH_ERROR_FLAGS) != 0U)
    {
        clearFlashErrors();
        return bsp::BSP_ERROR;
    }
    return bsp::BSP_OK;
}

bsp::BspReturnCode FlashEepromDriver::erasePage(uintptr_t pageAddr)
{
    if (unlockFlash() != bsp::BSP_OK)
    {
        return bsp::BSP_ERROR;
    }

    if (waitForFlash() != bsp::BSP_OK)
    {
        lockFlash();
        return bsp::BSP_ERROR;
    }
    clearFlashErrors();

#if defined(STM32G474xx)
    // STM32G4: page erase. Assumes the default dual-bank layout (DBANK = 1,
    // 2KB pages); pages 128 and above are in bank 2 and need BKER.
    uint32_t const page = static_cast<uint32_t>((pageAddr - FLASH_BASE) / fConfig.pageSize);
    uint32_t cr         = FLASH->CR & ~(FLASH_CR_PNB_Msk | FLASH_CR_BKER);
    cr |= ((page & 0x7FU) << FLASH_CR_PNB_Pos) | FLASH_CR_PER;
    if (page >= 128U)
    {
        cr |= FLASH_CR_BKER;
    }
    FLASH->CR = cr;
    FLASH->CR |= FLASH_CR_STRT;
#elif defined(STM32F413xx)
    // STM32F4: sector erase - the address must be a valid sector base
    uint32_t sector = 0U;
    if (!sectorFromPageAddress(pageAddr, sector))
    {
        lockFlash();
        return bsp::BSP_ERROR;
    }
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_SNB_Msk)) | (sector << FLASH_CR_SNB_Pos) | FLASH_CR_SER;
    FLASH->CR |= FLASH_CR_STRT;
#endif

    bsp::BspReturnCode result = waitForFlash();

#if defined(STM32G474xx)
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_STRT);
#elif defined(STM32F413xx)
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_STRT);
#endif

    lockFlash();
    return result;
}

bsp::BspReturnCode
FlashEepromDriver::programPage(uintptr_t destAddr, uint8_t const* data, uint32_t length)
{
    if (waitForFlash() != bsp::BSP_OK)
    {
        return bsp::BSP_ERROR;
    }
    clearFlashErrors();

#if defined(STM32G474xx)
    // STM32G4 flash requires double-word (64-bit) programming at
    // double-word-aligned addresses
    if ((destAddr & 0x7U) != 0U)
    {
        return bsp::BSP_ERROR;
    }
    FLASH->CR |= FLASH_CR_PG;

    uint32_t i = 0U;
    while (i < length)
    {
        uint32_t word0 = 0xFFFFFFFFU;
        uint32_t word1 = 0xFFFFFFFFU;

        uint32_t remaining = length - i;
        if (remaining > 0U)
        {
            std::memcpy(&word0, &data[i], (remaining >= 4U) ? 4U : remaining);
        }
        if (remaining > 4U)
        {
            uint32_t r2 = remaining - 4U;
            std::memcpy(&word1, &data[i + 4U], (r2 >= 4U) ? 4U : r2);
        }

        *reinterpret_cast<uint32_t volatile*>(destAddr + i)      = word0;
        *reinterpret_cast<uint32_t volatile*>(destAddr + i + 4U) = word1;

        if (waitForFlash() != bsp::BSP_OK)
        {
            FLASH->CR &= ~FLASH_CR_PG;
            return bsp::BSP_ERROR;
        }

        i += 8U;
    }

    FLASH->CR &= ~FLASH_CR_PG;
#elif defined(STM32F413xx)
    // STM32F4: word (32-bit) programming at word-aligned addresses, PSIZE=x32
    if ((destAddr & 0x3U) != 0U)
    {
        return bsp::BSP_ERROR;
    }
    FLASH->CR |= FLASH_CR_PG;
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PSIZE) | FLASH_CR_PSIZE_1;

    uint32_t i = 0U;
    while (i < length)
    {
        uint32_t word      = 0xFFFFFFFFU;
        uint32_t remaining = length - i;
        std::memcpy(&word, &data[i], (remaining >= 4U) ? 4U : remaining);

        *reinterpret_cast<uint32_t volatile*>(destAddr + i) = word;

        if (waitForFlash() != bsp::BSP_OK)
        {
            FLASH->CR &= ~FLASH_CR_PG;
            return bsp::BSP_ERROR;
        }

        i += 4U;
    }

    FLASH->CR &= ~FLASH_CR_PG;
#endif

    return bsp::BSP_OK;
}

} // namespace eeprom
