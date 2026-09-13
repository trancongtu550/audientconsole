#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DailyGui.h"
#include <windows.h>
#include <cmath>
#include <vector>
#include <string>
#include <chrono>
static float gLevelCh0 = -12.f, gLevelCh1=-8.f;
static bool gMute0=false, gMute1=false, gLocal0=true, gLocal1=true, gGlobal=true, gMono=false;
static double gMonHw=-18.5, gHpHw=-22.0;
int WINAPI wWinMain(HINSTANCE h, HINSTANCE, LPWSTR, int){
    using namespace audient::daily_gui;
    std::vector<std::string> ch1={"Auto-Tune Pro - Antares","SSL G-EQ - Waves","LA-2A - UAD"};
    std::vector<std::string> ch2={"Pro-Q 3 - FabFilter"};
    int sel1=0, sel2=0;
    bool ch1Bypass=false, ch2Bypass=false;
    DailyGuiConfig cfg;
    cfg.dualMode=true;
    cfg.userScale=1.f;
    cfg.onUserScaleChanged=[&](float s){cfg.userScale=s;};
    cfg.getGlobalMonitorOn=[&](){return gGlobal;};
    cfg.setGlobalMonitorOn=[&](bool v){gGlobal=v;};
    cfg.getMonoOn=[&](){return gMono;};
    cfg.setMonoOn=[&](bool v){gMono=v;};
    cfg.getLocalEnable=[&](int ch){return ch==0?gLocal0:gLocal1;};
    cfg.setLocalEnable=[&](int ch,bool v){if(ch==0)gLocal0=v;else gLocal1=v;};
    cfg.getLocalMute=[&](int ch){return ch==0?gMute0:gMute1;};
    cfg.setLocalMute=[&](int ch,bool v){if(ch==0)gMute0=v;else gMute1=v;};
    cfg.getLocalGainDb=[&](int ch){return ch==0?gLevelCh0:gLevelCh1;};
    cfg.setLocalGainDb=[&](int ch,float v){if(ch==0)gLevelCh0=v;else gLevelCh1=v;};
    auto t0=std::chrono::steady_clock::now();
    cfg.getMetersCh1=[&]()->std::pair<float,float>{
        auto dt=std::chrono::duration<float>(std::chrono::steady_clock::now()-t0).count();
        float raw = 0.35f + 0.30f*std::sin(dt*2.1f) + 0.10f*std::sin(dt*7.3f);
        float post = raw*0.85f;
        if(raw<0)raw=0; if(raw>1)raw=1;
        return {raw,post};
    };
    cfg.getMetersCh2=[&]()->std::pair<float,float>{
        auto dt=std::chrono::duration<float>(std::chrono::steady_clock::now()-t0).count();
        float raw = 0.25f + 0.25f*std::sin(dt*1.7f+1.2f);
        float post = raw*0.9f;
        if(raw<0)raw=0; if(raw>1)raw=1;
        return {raw,post};
    };
    cfg.getCh1Names=[&](){return ch1;};
    cfg.getCh2Names=[&](){return ch2;};
    cfg.getCh1Sel=[&](){return sel1;};
    cfg.getCh2Sel=[&](){return sel2;};
    cfg.setSel=[&](int ch,int idx){if(ch==0)sel1=idx;else sel2=idx;};
    cfg.onLoad=[&](int ch){ std::string nm="New Plug "+std::to_string((ch==0?ch1:ch2).size()+1); if(ch==0)ch1.push_back(nm); else ch2.push_back(nm); };
    cfg.onOpenEditor=[&](int,int){};
    cfg.onBypassSel=[&](int ch,int){ if(ch==0)ch1Bypass=!ch1Bypass; else ch2Bypass=!ch2Bypass; };
    cfg.onBypassWhole=[&](int ch){ if(ch==0)ch1Bypass=!ch1Bypass; else ch2Bypass=!ch2Bypass; };
    cfg.getWholeBypass=[&](int ch){return ch==0?ch1Bypass:ch2Bypass;};
    cfg.onRemove=[&](int ch,int idx){ if(ch==0 && idx>=0 && idx<(int)ch1.size()) ch1.erase(ch1.begin()+idx); if(ch==1 && idx>=0 && idx<(int)ch2.size()) ch2.erase(ch2.begin()+idx); };
    cfg.getAsioText=[&](){return std::string("ASIO Connected");};
    cfg.getXrunsText=[&](){return std::string("Xruns 0  Overloads 0");};
    cfg.getHwMonDb=[&]()->std::optional<double>{return gMonHw;};
    cfg.getHwHpDb=[&]()->std::optional<double>{return gHpHw;};
    cfg.getHwMonText=[&](){char b[48];snprintf(b,sizeof(b),"Mon HW %.1f dB",gMonHw);return std::string(b);};
    cfg.getHwHpText=[&](){char b[48];snprintf(b,sizeof(b),"HP HW %.1f dB",gHpHw);return std::string(b);};
    cfg.onNudgeHw=[&](bool hp,double d){ if(hp){gHpHw+=d; if(gHpHw<-127)gHpHw=-127; if(gHpHw>-6)gHpHw=-6;} else {gMonHw+=d; if(gMonHw<-127)gMonHw=-127; if(gMonHw>-6)gMonHw=-6;}};
    cfg.onSetHwDb=[&](bool hp,double v){ if(hp) gHpHw=v; else gMonHw=v; };
    HWND hwnd=Create(h,cfg);
    if(!hwnd) return 1;
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return 0;
}
