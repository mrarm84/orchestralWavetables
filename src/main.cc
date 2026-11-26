#include "dac.hh"
#include "system.hh"
#include "debug.hh"
#include "ui.hh"
#include "polyptic_oscillator.hh"
#include "dynamic_data.hh"

struct Main :
  System<kUiUpdateRate, Main>,
  Math,
  DynamicData,
  QSpiFlash,
  Dac<kSampleRate, kBlockSize, Main>,
  Ui<kUiUpdateRate, kBlockSize> {

  Main() {
    // cycle counter, used by the CPU load watchdog
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;      // unlock (Cortex-M7)
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    //Start audio processing
    Dac::Start();
    while(1) {
      Ui::Process();
      // TODO understand why this is crucial
      // just a "nop" is enough 
      __WFI();
    }
  }

  void SysTickCallback() {
    Ui::Update();
  }
  
  // Debug debug;

  template<int block_size>
  void DacCallback(Buffer<Frame, block_size>& out) {
    Process__IN_ITCM_(out);
  }

  // Audio block processing. "__IN_ITCM_" in the name makes the linker
  // place it in ITCM RAM (zero wait-state), like the ISR itself.
  [[gnu::noinline]] void Process__IN_ITCM_(Buffer<Frame, kBlockSize>& out) {
    // debug.set(3, true);
    uint32_t start = DWT->CYCCNT;
    Ui::Poll();
    Ui::osc().Process(out);
    Ui::report_load(DWT->CYCCNT - start);
    // debug.set(3, false);
  }
} _;
