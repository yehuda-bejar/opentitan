// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/math.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_aon_timer.h"
#include "sw/device/lib/dif/dif_clkmgr.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/dif/dif_rv_plic.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/alert_handler_testutils.h"
#include "sw/device/lib/testing/aon_timer_testutils.h"
#include "sw/device/lib/testing/rstmgr_testutils.h"
#include "sw/device/lib/testing/rv_plic_testutils.h"
#include "sw/device/lib/testing/test_framework/FreeRTOSConfig.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

/**
 * Program the alert handler to escalate on alerts upto phase 2 (i.e. reset).
 * Also program the aon timer with:
 * - bark after escalation starts, so the interrupt is suppressed by escalation,
 * - bite after escalation reset, so we should not get timer reset.
 */
enum {
  kWdogBarkMicros = 3 * 100,          // 300 us
  kWdogBiteMicros = 8 * 100,          // 800 us
  kEscalationPhase0Micros = 2 * 100,  // 200 us
  // The cpu value is set slightly larger, since if they are the same it is
  // possible busy_spin_micros in execute_test can complete before the
  // interrupt.
  kEscalationPhase0MicrosCpu = kEscalationPhase0Micros + 20,  // 220 us
  kEscalationPhase1Micros = 2 * 100,                          // 200 us
  kEscalationPhase2Micros = 100,                              // 100 us
};

static_assert(kWdogBarkMicros < kWdogBiteMicros &&
                  kWdogBarkMicros > kEscalationPhase0Micros &&
                  kWdogBarkMicros <
                      (kEscalationPhase0Micros + kEscalationPhase1Micros) &&
                  kWdogBiteMicros >
                      (kEscalationPhase0Micros + kEscalationPhase1Micros),
              "The wdog bite shall happen only if escalation reset fails.");

/**
 * Objects to access the peripherals used in this test via dif API.
 */
static const uint32_t kPlicTarget = kTopEarlgreyPlicTargetIbex0;
static dif_aon_timer_t aon_timer;
static dif_rv_plic_t plic;
static dif_clkmgr_t clkmgr;
static dif_rstmgr_t rstmgr;
static dif_alert_handler_t alert_handler;

volatile bool interrupt_seen = false;
const dif_alert_handler_alert_t expected_alert =
    kTopEarlgreyAlertIdClkmgrAonFatalFault;

/**
 * External ISR.
 *
 * Handles all peripheral interrupts on Ibex. PLIC asserts an external interrupt
 * line to the CPU, which results in a call to this OTTF ISR. This ISR
 * overrides the default OTTF implementation.
 */
void ottf_external_isr(void) {
  interrupt_seen = true;
  dif_rv_plic_irq_id_t irq_id;
  CHECK_DIF_OK(dif_rv_plic_irq_claim(&plic, kPlicTarget, &irq_id));

  top_earlgrey_plic_peripheral_t peripheral = (top_earlgrey_plic_peripheral_t)
      top_earlgrey_plic_interrupt_for_peripheral[irq_id];

  if (peripheral == kTopEarlgreyPlicPeripheralAonTimerAon) {
    uint32_t irq =
        (irq_id - (dif_rv_plic_irq_id_t)
                      kTopEarlgreyPlicIrqIdAonTimerAonWkupTimerExpired);

    // We should not get aon timer interrupts since escalation suppresses them.
    CHECK(false, "Unexpected aon timer interrupt %d", irq);
  } else if (peripheral == kTopEarlgreyPlicPeripheralAlertHandler) {
    // Check the class.
    dif_alert_handler_class_state_t state;
    CHECK_DIF_OK(dif_alert_handler_get_class_state(
        &alert_handler, kDifAlertHandlerClassA, &state));
    CHECK(state == kDifAlertHandlerClassStatePhase0, "Wrong phase %d", state);

    uint32_t irq =
        (irq_id -
         (dif_rv_plic_irq_id_t)kTopEarlgreyPlicIrqIdAlertHandlerClassa);

    // Check we get the expected alert.
    bool is_cause = false;
    CHECK_DIF_OK(dif_alert_handler_alert_is_cause(&alert_handler,
                                                  expected_alert, &is_cause));
    CHECK(is_cause);

    // Once this is handled, do not trigger interrupt again and wait for
    // system to escalate into reset.
    LOG_INFO("Disable IRQ classa");
    CHECK_DIF_OK(dif_alert_handler_irq_set_enabled(
        &alert_handler, kDifAlertHandlerIrqClassa, kDifToggleDisabled));
    // Fatal alerts are only cleared by reset.

    // Check the clkmgr has a fatal error.
    dif_clkmgr_fatal_err_codes_t codes;
    CHECK_DIF_OK(dif_clkmgr_fatal_err_code_get_codes(&clkmgr, &codes));
    CHECK(codes == kDifClkmgrFatalErrTypeIdleCount);

    CHECK_DIF_OK(dif_alert_handler_irq_acknowledge(&alert_handler, irq));
  }

  // Complete the IRQ by writing the IRQ source to the Ibex specific CC
  // register.
  CHECK_DIF_OK(dif_rv_plic_irq_complete(&plic, kPlicTarget, irq_id));
}

/**
 * Initialize the peripherals used in this test.
 */
void init_peripherals(void) {
  CHECK_DIF_OK(dif_clkmgr_init(
      mmio_region_from_addr(TOP_EARLGREY_CLKMGR_AON_BASE_ADDR), &clkmgr));

  CHECK_DIF_OK(dif_rstmgr_init(
      mmio_region_from_addr(TOP_EARLGREY_RSTMGR_AON_BASE_ADDR), &rstmgr));

  CHECK_DIF_OK(dif_aon_timer_init(
      mmio_region_from_addr(TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR), &aon_timer));

  CHECK_DIF_OK(dif_rv_plic_init(
      mmio_region_from_addr(TOP_EARLGREY_RV_PLIC_BASE_ADDR), &plic));

  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR),
      &alert_handler));
}

/**
 * Program the alert handler to escalate on alerts upto phase 2 (i.e. reset) but
 * the phase 1 (i.e. wipe secrets) should occur and last during the time the
 * wdog is programed to bark.
 */
static void alert_handler_config(void) {
  dif_alert_handler_alert_t alerts[] = {expected_alert};
  dif_alert_handler_class_t alert_classes[] = {kDifAlertHandlerClassA};

  dif_alert_handler_escalation_phase_t esc_phases[] = {
      {.phase = kDifAlertHandlerClassStatePhase0,
       .signal = 0,
       .duration_cycles = udiv64_slow(
           kEscalationPhase0Micros * kClockFreqPeripheralHz, 1000000,
           /*rem_out=*/NULL)},
      {.phase = kDifAlertHandlerClassStatePhase1,
       .signal = 1,
       .duration_cycles = udiv64_slow(
           kEscalationPhase1Micros * kClockFreqPeripheralHz, 1000000,
           /*rem_out=*/NULL)},
      {.phase = kDifAlertHandlerClassStatePhase2,
       .signal = 3,
       .duration_cycles = udiv64_slow(
           kEscalationPhase2Micros * kClockFreqPeripheralHz, 1000000,
           /*rem_out=*/NULL)}};

  dif_alert_handler_class_config_t class_config[] = {{
      .auto_lock_accumulation_counter = kDifToggleDisabled,
      .accumulator_threshold = 0,
      .irq_deadline_cycles =
          udiv64_slow(10 * kClockFreqPeripheralHz, 1000000, /*rem_out=*/NULL),
      .escalation_phases = esc_phases,
      .escalation_phases_len = ARRAYSIZE(esc_phases),
      .crashdump_escalation_phase = kDifAlertHandlerClassStatePhase3,
  }};

  dif_alert_handler_class_t classes[] = {kDifAlertHandlerClassA};
  dif_alert_handler_config_t config = {
      .alerts = alerts,
      .alert_classes = alert_classes,
      .alerts_len = ARRAYSIZE(alerts),
      .classes = classes,
      .class_configs = class_config,
      .classes_len = ARRAYSIZE(class_config),
      .ping_timeout = 0,
  };

  alert_handler_testutils_configure_all(&alert_handler, config,
                                        kDifToggleEnabled);
  // Enables alert handler irq.
  CHECK_DIF_OK(dif_alert_handler_irq_set_enabled(
      &alert_handler, kDifAlertHandlerIrqClassa, kDifToggleEnabled));
}

/**
 * Execute the aon timer interrupt test.
 */
static void execute_test(dif_aon_timer_t *aon_timer) {
  uint32_t bark_cycles =
      aon_timer_testutils_get_aon_cycles_from_us(kWdogBarkMicros);
  uint32_t bite_cycles =
      aon_timer_testutils_get_aon_cycles_from_us(kWdogBiteMicros);

  LOG_INFO(
      "Wdog will bark after %u/%u us/cycles and bite after %u/%u us/cycles",
      (uint32_t)kWdogBarkMicros, bark_cycles, (uint32_t)kWdogBiteMicros,
      bite_cycles);

  // Setup the wdog bark and bite timeouts.
  aon_timer_testutils_watchdog_config(aon_timer, bark_cycles, bite_cycles,
                                      /*pause_in_sleep=*/false);

  // Trigger the clkmgr fatal error to start escalation.
  LOG_INFO("Ready for error injection");
  abort();
}

bool test_main(void) {
  // Enable global and external IRQ at Ibex.
  irq_global_ctrl(true);
  irq_external_ctrl(true);

  init_peripherals();

  // Enable all the interrupts used in this test.
  rv_plic_testutils_irq_range_enable(
      &plic, kPlicTarget, kTopEarlgreyPlicIrqIdAonTimerAonWkupTimerExpired,
      kTopEarlgreyPlicIrqIdAonTimerAonWdogTimerBark);
  rv_plic_testutils_irq_range_enable(&plic, kPlicTarget,
                                     kTopEarlgreyPlicIrqIdAlertHandlerClassa,
                                     kTopEarlgreyPlicIrqIdAlertHandlerClassd);

  alert_handler_config();

  // Check if there was a HW reset caused by the escalation.
  dif_rstmgr_reset_info_bitfield_t rst_info;
  rst_info = rstmgr_testutils_reason_get();
  rstmgr_testutils_reason_clear();

  CHECK(rst_info == kDifRstmgrResetInfoPor ||
            rst_info == kDifRstmgrResetInfoEscalation,
        "Wrong reset reason %02X", rst_info);

  if (rst_info == kDifRstmgrResetInfoPor) {
    LOG_INFO("Booting for the first time, starting test");
    execute_test(&aon_timer);
  } else if (rst_info == kDifRstmgrResetInfoEscalation) {
    LOG_INFO("Booting for the second time due to escalation reset");
    // Check the clkmgr has no fatal errors due to reset.
    dif_clkmgr_fatal_err_codes_t codes;
    CHECK_DIF_OK(dif_clkmgr_fatal_err_code_get_codes(&clkmgr, &codes));
    CHECK(codes == 0, "Fatal error codes should be cleared upon reset");
    // Check the alert handler cause is also cleared.
    bool is_cause = true;
    CHECK_DIF_OK(dif_alert_handler_alert_is_cause(&alert_handler,
                                                  expected_alert, &is_cause));
    CHECK(!is_cause);
    // Turn off the AON timer hardware completely before exiting.
    aon_timer_testutils_shutdown(&aon_timer);
    return true;

  } else {
    LOG_ERROR("Unexpected rst_info=0x%x", rst_info);
    return false;
  }

  return false;
}
