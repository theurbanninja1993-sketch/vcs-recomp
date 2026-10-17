#include "MiniTest.h"
#include <cstdlib>
#include <iostream>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_ps2_gs_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_sif_dma_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void register_ps2_mods_tests();
void reset_ps2_test_function_table();

int main()
{
    // The GS tests check the CPU rasterizer's behaviour (VRAM straight after a
    // draw, its texture and blend paths). The port defaults to the GPU renderer
    // since 37b0bd4, which silently moved these tests onto it and failed 25 of
    // them; pin the CPU renderer unless PS2X_GS_GPU is already set. Likewise
    // pin its synchronous mode: the threaded rasterizer (f9774e2) hands each
    // present back one latch later, so a test that latches once got no frame
    // and then read past an empty buffer (the suite segfaulted). The async VIF1
    // worker does the same to the DMA tests, which expect a transfer to be done
    // when processPendingTransfers() returns. The VU1 tests check the exact
    // pipeline timing the game skips by default (PS2X_VU1_EXACT_PIPELINE), and
    // the interlaced presentation test the line-doubled field output the port
    // replaced with full frames (65e22b2; PS2X_FIELD_BOB). The texture page
    // buffer test needs the snapshotting texture cache (PS2X_GS_TEXCACHE),
    // which the port replaced with direct VRAM reads (5abf73e).
#if defined(_WIN32)
    if (std::getenv("PS2X_GS_GPU") == nullptr)
        _putenv_s("PS2X_GS_GPU", "0");
    if (std::getenv("PS2X_GS_SYNC") == nullptr)
        _putenv_s("PS2X_GS_SYNC", "1");
    if (std::getenv("PS2X_SYNC_VIF1") == nullptr)
        _putenv_s("PS2X_SYNC_VIF1", "1");
    if (std::getenv("PS2X_VU1_EXACT_PIPELINE") == nullptr)
        _putenv_s("PS2X_VU1_EXACT_PIPELINE", "1");
    if (std::getenv("PS2X_FIELD_BOB") == nullptr)
        _putenv_s("PS2X_FIELD_BOB", "1");
    if (std::getenv("PS2X_GS_TEXCACHE") == nullptr)
        _putenv_s("PS2X_GS_TEXCACHE", "1");
#else
    setenv("PS2X_GS_GPU", "0", 0);
    setenv("PS2X_GS_SYNC", "1", 0);
    setenv("PS2X_SYNC_VIF1", "1", 0);
    setenv("PS2X_VU1_EXACT_PIPELINE", "1", 0);
    setenv("PS2X_FIELD_BOB", "1", 0);
    setenv("PS2X_GS_TEXCACHE", "1", 0);
#endif
    MiniTest::BeforeEach(reset_ps2_test_function_table);

    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_pad_input_tests();
    register_ps2_runtime_io_tests();
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_memory_tests();
    register_ps2_vu1_tests();
    register_ps2_vu_tests();
    register_ps2_gs_tests();
    register_ps2_iop_tests();
    register_ps2_sif_rpc_tests();
    register_ps2_sif_dma_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
    register_ps2_mods_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
