/********************************************************************************
 * Copyright (c) 2026 An Dao
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

#include <adc/Adc.h>

namespace
{
// Iteration bound for status-flag polls. Calibration is the slowest bounded
// operation (hundreds of ADC clock cycles); one million iterations is orders
// of magnitude above that at any supported core clock, so hitting the bound
// means the ADC is mis-clocked or faulted and an error is returned instead
// of spinning forever.
uint32_t const FLAG_TIMEOUT = 1000000U;
} // namespace

namespace bios
{

Adc::Adc(AdcConfig const& config) : fConfig(config), fInitialized(false) {}

void Adc::enableClock()
{
#if defined(STM32G474xx)
    RCC->AHB2ENR |= RCC_AHB2ENR_ADC12EN;
    uint32_t volatile dummy = RCC->AHB2ENR;
    (void)dummy;

    // Select the system clock as ADC kernel clock (ADC12SEL = 10). Note that
    // 01 would select the PLL "P" output, which the clock configuration does
    // not enable.
    RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_ADC12SEL) | RCC_CCIPR_ADC12SEL_1;
#elif defined(STM32F413xx)
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    uint32_t volatile dummy = RCC->APB2ENR;
    (void)dummy;
#endif
}

bsp::BspReturnCode Adc::calibrate()
{
#if defined(STM32G474xx)
    ADC_TypeDef* adc = fConfig.peripheral;

    adc->CR &= ~ADC_CR_ADEN;
    adc->CR &= ~ADC_CR_DEEPPWD;
    adc->CR |= ADC_CR_ADVREGEN;

    // Wait for the voltage regulator startup time T_ADCVREG_STUP (20 us).
    // A volatile counting loop takes at least 3 cycles per iteration, so
    // 4000 iterations is > 70 us at 170 MHz and proportionally longer at
    // lower core clocks.
    for (uint32_t volatile i = 0U; i < 4000U; i++) {}

    adc->CR &= ~ADC_CR_ADCALDIF;
    adc->CR |= ADC_CR_ADCAL;
    uint32_t count = 0U;
    while ((adc->CR & ADC_CR_ADCAL) != 0U)
    {
        if (++count > FLAG_TIMEOUT)
        {
            return bsp::BSP_ERROR;
        }
    }
    return bsp::BSP_OK;
#elif defined(STM32F413xx)
    // STM32F4 ADC has no hardware calibration sequence
    fConfig.peripheral->CR2 &= ~ADC_CR2_ADON;
    return bsp::BSP_OK;
#else
    return bsp::BSP_ERROR;
#endif
}

bsp::BspReturnCode Adc::init()
{
    enableClock();
    if (calibrate() != bsp::BSP_OK)
    {
        return bsp::BSP_ERROR;
    }

    ADC_TypeDef* adc = fConfig.peripheral;

#if defined(STM32G474xx)
    adc->CFGR = (adc->CFGR & ~ADC_CFGR_RES)
                | (static_cast<uint32_t>(fConfig.resolution) << ADC_CFGR_RES_Pos);
    adc->CFGR &= ~(ADC_CFGR_CONT | ADC_CFGR_EXTEN);
    adc->CFGR &= ~ADC_CFGR_ALIGN;

    // Connect the internal temperature sensor (ADC1 IN16) and VREFINT
    // (ADC1 IN18) paths.
    ADC12_COMMON->CCR |= ADC_CCR_VSENSESEL | ADC_CCR_VREFEN;

    adc->ISR |= ADC_ISR_ADRDY; // Write-1-to-clear ready flag
    adc->CR |= ADC_CR_ADEN;
    uint32_t count = 0U;
    while ((adc->ISR & ADC_ISR_ADRDY) == 0U)
    {
        if (++count > FLAG_TIMEOUT)
        {
            return bsp::BSP_ERROR;
        }
    }
#elif defined(STM32F413xx)
    adc->CR1 = (adc->CR1 & ~ADC_CR1_RES)
               | (static_cast<uint32_t>(fConfig.resolution) << ADC_CR1_RES_Pos);
    adc->CR2 &= ~(ADC_CR2_CONT | ADC_CR2_ALIGN);

    ADC_Common_TypeDef* common = ADC1_COMMON;
    common->CCR                = (common->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0; // PCLK2/4
    // Connect the internal temperature sensor (IN18) and VREFINT (IN17) paths.
    common->CCR |= ADC_CCR_TSVREFE;

    adc->CR2 |= ADC_CR2_ADON;
#endif

    fInitialized = true;
    return bsp::BSP_OK;
}

void Adc::configureChannel(uint8_t channel)
{
    ADC_TypeDef* adc = fConfig.peripheral;

#if defined(STM32G474xx)
    adc->SQR1 = (adc->SQR1 & ~(ADC_SQR1_L | ADC_SQR1_SQ1))
                | (static_cast<uint32_t>(channel) << ADC_SQR1_SQ1_Pos); // L=0 (1 conv)

    // SMPR1 covers channels 0-9 (3 bits each), SMPR2 covers 10-18
    if (channel < 10U)
    {
        uint32_t pos = channel * 3U;
        adc->SMPR1
            = (adc->SMPR1 & ~(7U << pos)) | (static_cast<uint32_t>(fConfig.samplingTime) << pos);
    }
    else
    {
        uint32_t pos = (channel - 10U) * 3U;
        adc->SMPR2
            = (adc->SMPR2 & ~(7U << pos)) | (static_cast<uint32_t>(fConfig.samplingTime) << pos);
    }
#elif defined(STM32F413xx)
    adc->SQR1 &= ~ADC_SQR1_L;                                           // L=0 (1 conv)
    adc->SQR3 = (adc->SQR3 & ~ADC_SQR3_SQ1) | (channel & ADC_SQR3_SQ1); // SQ1=channel

    // F4: SMPR2 covers channels 0-9, SMPR1 covers 10-18 (reversed vs G4)
    if (channel < 10U)
    {
        uint32_t pos = channel * 3U;
        adc->SMPR2
            = (adc->SMPR2 & ~(7U << pos)) | (static_cast<uint32_t>(fConfig.samplingTime) << pos);
    }
    else
    {
        uint32_t pos = (channel - 10U) * 3U;
        adc->SMPR1
            = (adc->SMPR1 & ~(7U << pos)) | (static_cast<uint32_t>(fConfig.samplingTime) << pos);
    }
#endif
}

bsp::BspReturnCode Adc::startAndRead(uint16_t& value)
{
    ADC_TypeDef* adc = fConfig.peripheral;
    uint32_t count   = 0U;

#if defined(STM32G474xx)
    adc->ISR |= ADC_ISR_EOC; // Write-1-to-clear EOC
    adc->CR |= ADC_CR_ADSTART;
    while ((adc->ISR & ADC_ISR_EOC) == 0U)
    {
        if (++count > FLAG_TIMEOUT)
        {
            return bsp::BSP_ERROR;
        }
    }
    value = static_cast<uint16_t>(adc->DR);
    return bsp::BSP_OK;
#elif defined(STM32F413xx)
    adc->SR &= ~ADC_SR_EOC;
    adc->CR2 |= ADC_CR2_SWSTART;
    while ((adc->SR & ADC_SR_EOC) == 0U)
    {
        if (++count > FLAG_TIMEOUT)
        {
            return bsp::BSP_ERROR;
        }
    }
    value = static_cast<uint16_t>(adc->DR);
    return bsp::BSP_OK;
#else
    (void)count;
    (void)value;
    return bsp::BSP_ERROR;
#endif
}

bsp::BspReturnCode Adc::readChannel(uint8_t channel, uint16_t& value)
{
    if (!fInitialized)
    {
        return bsp::BSP_ERROR;
    }
    configureChannel(channel);
    return startAndRead(value);
}

bsp::BspReturnCode Adc::readTemperature(uint16_t& value)
{
#if defined(STM32G474xx)
    return readChannel(16U, value); // VSENSE on ADC1 ch16
#elif defined(STM32F413xx)
    return readChannel(18U, value); // VSENSE on ADC1 ch18
#else
    (void)value;
    return bsp::BSP_ERROR;
#endif
}

bsp::BspReturnCode Adc::readVrefint(uint16_t& value)
{
#if defined(STM32G474xx)
    return readChannel(18U, value); // VREFINT on ADC1 ch18
#elif defined(STM32F413xx)
    return readChannel(17U, value); // VREFINT on ADC1 ch17
#else
    (void)value;
    return bsp::BSP_ERROR;
#endif
}

} // namespace bios
