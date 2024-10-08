#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_oldnames.h>
#include <SDL3/SDL_video.h>
#include <chrono>
#include <cstdlib>
#include <lib/types.h>
#include <core/mmu.h>
#include <core/timer.h>
#include <core/core.h>
#include <core/ppu.h>
#include <gb.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <thread>

GB::GB() : joypad(), mem(joypad), core(mem), timer(mem), ppu(mem), apu(mem),
    window(SDL_CreateWindow("geebeemoo", 160, 144, 0)) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);

    
    if (!window) {
        std::cout << "error creating window " << SDL_GetError() << "\n"; 
        exit(-1);
    }
    //SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    surface = SDL_GetWindowSurface(window);
    if (!surface) {
        std::cout << "error creating surface " << SDL_GetError() << "\n"; 
        exit(-1);
    }

    ppu.setSurface(surface);

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 48000;
    want.format = SDL_AUDIO_F32;
    want.channels = 1;

    dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);

    constexpr SDL_AudioSpec src = { SDL_AUDIO_F32, 1, 262144 };
    constexpr SDL_AudioSpec dst = { SDL_AUDIO_F32, 1, 48000 };

    audio_stream = SDL_CreateAudioStream(&src, &dst);
    if (!audio_stream) {
        std::cout << "error creating audio stream " << SDL_GetError() << "\n"; 
        exit(-1);
    }
    if (!SDL_BindAudioStream(dev, audio_stream)) {
        std::cout << "failed to bind audio stream\n";
        exit(-1);
    }
    apu.setAudioStream(audio_stream);
}

GB::~GB() {
    SDL_DestroyAudioStream(audio_stream);
    SDL_CloseAudioDevice(dev);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void callback(void* apu_ptr, u8* stream, int len) {
    /*
    auto* float_stream{std::bit_cast<float*>(stream)};
    float sample = 0;
    APU& apu = *(std::bit_cast<APU*>(apu_ptr)); // lol???? ????? ???
    len /= sizeof(float); // LOL!!
    for (auto i = 0; i < len; ++i) {
        sample = apu.getSample();
        float_stream[i] = 0.1f * sample;
    }
    */

}

void GB::runEmu(char* filename) {
    // putting 60 instead of the actual value makes it slightly more accurate lol
    const double FPS = 60;
    std::chrono::duration<double, std::micro> frameDelay(1000000 / FPS);
    const u32 maxTicks = 70224; // number of instuctions per frame
    u32 current_ticks = maxTicks;
    std::chrono::time_point<std::chrono::high_resolution_clock> frameStart;
    std::chrono::duration<double, std::micro> frameTime{};
    u32 div_ticks = 0;
    u32 operation_ticks = 0;
    bool tima_flag = false;

    // these should probably actually be member variables
    if (0 == mem.load_cart(filename)) {
        std::cout << "emu quitting due to rom not existing\n";
        return;
    }
    bool running = true;
    bool first_frame = true;
    bool white = false;

#ifdef OLD
    std::ofstream log("oldlog.txt", std::ofstream::trunc);
#else
    std::ofstream log("newlog.txt", std::ofstream::trunc);
#endif
    u32 frame = 1;
    std::chrono::duration<double, std::micro> frameavg{};


    core.bootup();
    apu.initAPU();

    SDL_Event event;

    frameStart = std::chrono::high_resolution_clock::now();
    constexpr static std::array<u8,4> tima_freq = { 9, 3, 5, 7 };
    while(running) {
        current_ticks = current_ticks - maxTicks;
        div_ticks = 0;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else joypad.pollPresses(event);
        }
        white = false;
        while (current_ticks < maxTicks) {
            u16 div = (mem.read(0xFF04) << 8) + mem.read(0xFF03);
            u8 tima_bit = (div >> tima_freq[mem.read(0xFF07) & 0b11]) & 0b1;
#ifdef DEBUG
            doctor_log(frame, current_ticks, log, core, mem);
#endif
            operation_ticks = core.op_tree();
            current_ticks += operation_ticks;
            if (mem.get_oam()) {
                mem.oam_transfer(current_ticks);
            }
            if ((mem.hw_read(0xFF40) & 0x80) == 0x80) {
                ppu.ppuLoop(operation_ticks);
            } else { // lcd disable
                mem.hw_write(0xFF44, (u8)0);
                mem.hw_write(0xFF41, (u8)((mem.hw_read(0xFF41) & (u8)0b11111100) | (u8)mode0));
                ppu.currentLineDots = 0;
                white = true;
            }
            div_ticks += operation_ticks;
            while (div_ticks >= 4) {
                timer.div_inc();
                apu.period_clock();
                div = (mem.read(0xFF04) << 8) + mem.read(0xFF03);
                u8 after_tima_bit = (div >> tima_freq[mem.read(0xFF07) & 0b11]) & 0b1; 
                if ((mem.read(0xFF07) > 3 && tima_flag) || ((tima_bit == 1) && (after_tima_bit == 0) && mem.read(0xFF07) > 3)) { // falling edge
                    tima_flag = (timer.tima_inc() == -1);
                }
                div_ticks -= 4;
                div = (mem.read(0xFF04) << 8) + mem.read(0xFF03);
                tima_bit = (div >> tima_freq[mem.read(0xFF07) & 0b11]) & 0b1;
            }
        }
        if (first_frame) {
            first_frame = false;
            SDL_FillSurfaceRect(surface, nullptr, 0xFFFFFFFF);
        } else if (white) {
            SDL_FillSurfaceRect(surface, nullptr, 0xFFFFFFFF);
        }
        frameTime = std::chrono::high_resolution_clock::now().time_since_epoch() - frameStart.time_since_epoch();
        if (frameDelay > frameTime) std::this_thread::sleep_for(std::chrono::duration(frameDelay - frameTime));
        frame += 1;
        frameavg += std::chrono::high_resolution_clock::now().time_since_epoch() - frameStart.time_since_epoch();;
        //std::cout << std::dec << (double)(std::chrono::high_resolution_clock::now().time_since_epoch() - frameStart.time_since_epoch()).count() / 1000000 << " ms for frame " << (int) frame << "\n";
        //assert(mem.read(0xFF44) >= 153);
        SDL_UpdateWindowSurface(window);
        
        /*
        if (SDL_GetAudioStreamAvailable(audio_stream) >= 4 * 48000) {
            std::array<float, 48000> buffer{};
            SDL_GetAudioStreamData(audio_stream, &buffer[0], 4 * 48000);
            for (auto sample : buffer) {
                std::cout << sample << "\n";
            }
            exit(0);
        }
        */
        
        frameStart = std::chrono::high_resolution_clock::now();
    } 
    std::cout << SDL_GetError();
    std::cout << "\n" << frameavg.count() / 1000 / frame << " avg ms per frame\n";
    std::cout << 1000000 / frameavg.count() * frame << " avg fps\n";
    std::cout << "closing gbemu\n";
}

void GB::doctor_log(u32 frame, u32 ticks, std::ofstream& log, Core& core, MMU& mem) {
    log << "Frame: " << std::dec << (int)frame;
    log << " Ticks: " << std::dec << (int)ticks;
    log << std::hex << std::setfill('0') << " A:" << std::setw(2) << (int) core.registers.gpr.n.a;
    log << " F:" << std::setw(2) << (int) core.registers.flags;
    log << " B:" <<  std::setw(2) << (int) core.registers.gpr.n.b;
    log << " C:" <<  std::setw(2) << (int) core.registers.gpr.n.c;
    log << " D:" <<  std::setw(2) << (int) core.registers.gpr.n.d;
    log << " E:" <<  std::setw(2) << (int) core.registers.gpr.n.e;
    log << " H:" <<  std::setw(2) << (int) core.registers.gpr.n.h;
    log << " L:" <<  std::setw(2) << (int) core.registers.gpr.n.l;
    log << " SP:" <<  std::setw(4) << (int) core.registers.sp;
    log << " SPMEM:" <<  std::setw(4) << (int) ((mem.hw_read(core.registers.sp + 1) << 8) + mem.hw_read(core.registers.sp));
    log << " PC:" <<  std::setw(4) << (int) core.registers.pc;
    log << " PCMEM:" <<  std::setw(2) << (int) mem.read(core.registers.pc) << ",";
    log <<  std::setw(2) << (int) mem.read(core.registers.pc + 1) << ",";
    log <<  std::setw(2) << (int) mem.read(core.registers.pc + 2) << ",";
    log <<  std::setw(2) << (int) mem.read(core.registers.pc + 3);
    log << "\n";
}
