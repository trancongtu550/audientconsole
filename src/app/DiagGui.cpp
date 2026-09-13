#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DiagGui.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#pragma comment(lib, "comctl32.lib")

static const wchar_t* kCls = L"AudientDiagPanel";
static DiagGuiConfig* gCfg = nullptr;
static HWND gHwnd = nullptr;

enum Id : int {
    ID_CH1_RAW=1001, ID_CH1_POST, ID_CH1_EN, ID_CH1_MUTE, ID_CH1_GAIN_TXT, ID_CH1_GAIN_TRK,
    ID_CH1_LIST, ID_CH1_LOAD, ID_CH1_EDITOR, ID_CH1_BYP, ID_CH1_WHOLE, ID_CH1_REMOVE,
    ID_CH2_RAW, ID_CH2_POST, ID_CH2_EN, ID_CH2_MUTE, ID_CH2_GAIN_TXT, ID_CH2_GAIN_TRK,
    ID_CH2_LIST, ID_CH2_LOAD, ID_CH2_EDITOR, ID_CH2_BYP, ID_CH2_WHOLE, ID_CH2_REMOVE,
    ID_GLOBAL_MON, ID_MONO, ID_ASIO_TXT, ID_XRUN_TXT, ID_HW_MON_TXT, ID_HW_HP_TXT,
    ID_HW_MON_TRK, ID_HW_HP_TRK,
    ID_HW_MON_UP, ID_HW_MON_DN, ID_HW_HP_UP, ID_HW_HP_DN,
    ID_TIMER=9001
};

static constexpr double kHwFloor = -127.0;
static constexpr double kHwCeil = -6.0;
static constexpr int kHwSteps = 121;

static std::string fmtDb(float peak){
    if(peak<=0.0001f) return "-inf";
    char b[32]; std::snprintf(b,sizeof(b),"%.1f",20.0*std::log10((double)peak));
    return b;
}
static std::string dbTextFromPeak(float p){ return fmtDb(p)+" dBFS"; }
static float trkToDb(int pos){ return (float)pos - 60.f; }
static int dbToTrk(float db){ int v=(int)std::lround(db+60.f); if(v<0)v=0; if(v>60)v=60; return v; }
static int hwDbToPos(double db){
    if(!std::isfinite(db)) db=kHwFloor;
    if(db<kHwFloor) db=kHwFloor;
    if(db>kHwCeil) db=kHwCeil;
    int p=(int)std::lround(db - kHwFloor);
    if(p<0) p=0; if(p>kHwSteps) p=kHwSteps;
    return p;
}
static double posToHwDb(int pos){
    if(pos<0) pos=0; if(pos>kHwSteps) pos=kHwSteps;
    return kHwFloor + (double)pos;
}


struct MeterBallistics {
    float displayed = 0.0f;
    float hold = 0.0f;
    std::chrono::steady_clock::time_point holdUntil{};
    std::chrono::steady_clock::time_point lastTick = std::chrono::steady_clock::now();
    float update(float newPeak){
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTick).count();
        if(dt < 0.0f) dt = 0.0f;
        if(dt > 0.5f) dt = 0.5f;
        lastTick = now;
        const float kReleaseDbPerSec = 20.0f;
        const float kHoldSec = 0.4f;
        if(newPeak > displayed){
            displayed = newPeak;
            hold = newPeak;
            holdUntil = now + std::chrono::milliseconds((int)(kHoldSec*1000));
        } else {
            if(displayed > 1e-6f){
                float db = 20.0f * std::log10(displayed);
                db -= kReleaseDbPerSec * dt;
                float decayed = std::pow(10.0f, db/20.0f);
                if(decayed < newPeak) decayed = newPeak;
                if(decayed < 1e-5f) decayed = 0.0f;
                displayed = decayed;
            } else {
                displayed = newPeak;
            }
            if(now >= holdUntil) hold = displayed;
            else if(newPeak > hold) { hold = newPeak; holdUntil = now + std::chrono::milliseconds((int)(kHoldSec*1000)); }
        }
        return displayed;
    }
};
static MeterBallistics gM1Raw, gM1Post, gM2Raw, gM2Post;

static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{
        gHwnd=h;
        auto add=[&](const wchar_t* t,int x,int y,int w2,int h2,int id,DWORD style){
            CreateWindowExW(0,L"STATIC",t,WS_CHILD|WS_VISIBLE|style,x,y,w2,h2,h,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
        };
        auto addBtn=[&](const wchar_t* t,int x,int y,int w2,int h2,int id){
            CreateWindowW(L"BUTTON",t,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,w2,h2,h,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
        };
        auto addChk=[&](const wchar_t* t,int x,int y,int w2,int h2,int id){
            CreateWindowW(L"BUTTON",t,WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x,y,w2,h2,h,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
        };
        int y=8;
        CreateWindowW(L"STATIC",L"CH1",WS_CHILD|WS_VISIBLE|SS_LEFT,8,y,40,14,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        add(L"RAW -inf",8,y+14,150,14,ID_CH1_RAW,SS_LEFT);
        add(L"POST -inf",170,y+14,150,14,ID_CH1_POST,SS_LEFT);
        addChk(L"Local Enable",8,y+30,110,16,ID_CH1_EN);
        addChk(L"Mute",130,y+30,60,16,ID_CH1_MUTE);
        add(L"Gain -18.0 dB",200,y+30,90,14,ID_CH1_GAIN_TXT,SS_LEFT);
        CreateWindowW(TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ,300,y+28,160,20,h,(HMENU)ID_CH1_GAIN_TRK,GetModuleHandleW(nullptr),nullptr);
        SendDlgItemMessageW(h,ID_CH1_GAIN_TRK,TBM_SETRANGE,TRUE,MAKELPARAM(0,60));
        SendDlgItemMessageW(h,ID_CH1_GAIN_TRK,TBM_SETPOS,TRUE,dbToTrk(-18));
        CreateWindowW(L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY,8,y+52,240,70,h,(HMENU)ID_CH1_LIST,GetModuleHandleW(nullptr),nullptr);
        addBtn(L"Load",260,y+52,70,22,ID_CH1_LOAD);
        addBtn(L"Editor",340,y+52,70,22,ID_CH1_EDITOR);
        addBtn(L"BypSel",260,y+76,70,22,ID_CH1_BYP);
        addBtn(L"Whole",340,y+76,70,22,ID_CH1_WHOLE);
        addBtn(L"Remove",300,y+100,70,22,ID_CH1_REMOVE);
        y=150;
        CreateWindowW(L"STATIC",L"CH2",WS_CHILD|WS_VISIBLE|SS_LEFT,8,y,40,14,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        add(L"RAW -inf",8,y+14,150,14,ID_CH2_RAW,SS_LEFT);
        add(L"POST -inf",170,y+14,150,14,ID_CH2_POST,SS_LEFT);
        addChk(L"Local Enable",8,y+30,110,16,ID_CH2_EN);
        addChk(L"Mute",130,y+30,60,16,ID_CH2_MUTE);
        add(L"Gain -18.0 dB",200,y+30,90,14,ID_CH2_GAIN_TXT,SS_LEFT);
        CreateWindowW(TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ,300,y+28,160,20,h,(HMENU)ID_CH2_GAIN_TRK,GetModuleHandleW(nullptr),nullptr);
        SendDlgItemMessageW(h,ID_CH2_GAIN_TRK,TBM_SETRANGE,TRUE,MAKELPARAM(0,60));
        SendDlgItemMessageW(h,ID_CH2_GAIN_TRK,TBM_SETPOS,TRUE,dbToTrk(-18));
        CreateWindowW(L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY,8,y+52,240,70,h,(HMENU)ID_CH2_LIST,GetModuleHandleW(nullptr),nullptr);
        addBtn(L"Load",260,y+52,70,22,ID_CH2_LOAD);
        addBtn(L"Editor",340,y+52,70,22,ID_CH2_EDITOR);
        addBtn(L"BypSel",260,y+76,70,22,ID_CH2_BYP);
        addBtn(L"Whole",340,y+76,70,22,ID_CH2_WHOLE);
        addBtn(L"Remove",300,y+100,70,22,ID_CH2_REMOVE);
        if(gCfg && !gCfg->dualMode){
            for(int id:{ID_CH2_EN,ID_CH2_MUTE,ID_CH2_GAIN_TRK,ID_CH2_LOAD,ID_CH2_EDITOR,ID_CH2_BYP,ID_CH2_WHOLE,ID_CH2_REMOVE})
                EnableWindow(GetDlgItem(h,id),FALSE);
            EnableWindow(GetDlgItem(h,ID_CH2_LIST),FALSE);
        }
        y=290;
        addChk(L"Global Local Monitor ON",8,y,190,16,ID_GLOBAL_MON);
        addChk(L"Mono ON",210,y,90,16,ID_MONO);
        add(L"ASIO --",8,y+18,260,14,ID_ASIO_TXT,SS_LEFT);
        add(L"Xruns --",280,y+18,200,14,ID_XRUN_TXT,SS_LEFT);
        add(L"Monitor HW --",8,y+34,150,14,ID_HW_MON_TXT,SS_LEFT);
        CreateWindowW(TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ,165,y+32,180,20,h,(HMENU)ID_HW_MON_TRK,GetModuleHandleW(nullptr),nullptr);
        SendDlgItemMessageW(h,ID_HW_MON_TRK,TBM_SETRANGE,TRUE,MAKELPARAM(0,kHwSteps));
        SendDlgItemMessageW(h,ID_HW_MON_TRK,TBM_SETPOS,TRUE,hwDbToPos(kHwFloor));
        addBtn(L"-1",350,y+32,32,18,ID_HW_MON_DN);
        addBtn(L"+1",385,y+32,32,18,ID_HW_MON_UP);
        add(L"Headphone HW --",8,y+54,150,14,ID_HW_HP_TXT,SS_LEFT);
        CreateWindowW(TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ,165,y+52,180,20,h,(HMENU)ID_HW_HP_TRK,GetModuleHandleW(nullptr),nullptr);
        SendDlgItemMessageW(h,ID_HW_HP_TRK,TBM_SETRANGE,TRUE,MAKELPARAM(0,kHwSteps));
        SendDlgItemMessageW(h,ID_HW_HP_TRK,TBM_SETPOS,TRUE,hwDbToPos(kHwFloor));
        addBtn(L"-1",350,y+52,32,18,ID_HW_HP_DN);
        addBtn(L"+1",385,y+52,32,18,ID_HW_HP_UP);
        SetTimer(h,ID_TIMER,120,nullptr);
        PostMessageW(h,WM_TIMER,ID_TIMER,0);
        return 0;
    }
    case WM_TIMER:{
        if(!gCfg) break;
        auto upd=[&](int idRaw,int idPost,std::function<std::pair<float,float>()> fn, MeterBallistics& bRaw, MeterBallistics& bPost){
            if(!fn) return;
            auto [raw,post]=fn();
            float dRaw = bRaw.update(raw);
            float dPost = bPost.update(post);
            std::string r="RAW "+dbTextFromPeak(dRaw);
            std::string p="POST "+dbTextFromPeak(dPost);
            if(bRaw.hold > dRaw*1.02f && bRaw.hold > 0.001f){ char hb[32]; std::snprintf(hb,sizeof(hb)," pk %.1f",20.0*std::log10(bRaw.hold)); r+=hb; }
            if(bPost.hold > dPost*1.02f && bPost.hold > 0.001f){ char hb[32]; std::snprintf(hb,sizeof(hb)," pk %.1f",20.0*std::log10(bPost.hold)); p+=hb; }
            SetWindowTextA(GetDlgItem(h,idRaw),r.c_str());
            SetWindowTextA(GetDlgItem(h,idPost),p.c_str());
        };
        upd(ID_CH1_RAW,ID_CH1_POST,gCfg->getMetersCh1, gM1Raw, gM1Post);
        upd(ID_CH2_RAW,ID_CH2_POST,gCfg->getMetersCh2, gM2Raw, gM2Post);
        if(gCfg->getGlobalMonitorOn) CheckDlgButton(h,ID_GLOBAL_MON,gCfg->getGlobalMonitorOn()?BST_CHECKED:BST_UNCHECKED);
        if(gCfg->getMonoOn) CheckDlgButton(h,ID_MONO,gCfg->getMonoOn()?BST_CHECKED:BST_UNCHECKED);
        for(int ch=0;ch<2;++ch){
            int idEn= ch==0?ID_CH1_EN:ID_CH2_EN;
            int idMu= ch==0?ID_CH1_MUTE:ID_CH2_MUTE;
            if(gCfg->getLocalEnable) CheckDlgButton(h,idEn,gCfg->getLocalEnable(ch)?BST_CHECKED:BST_UNCHECKED);
            if(gCfg->getLocalMute) CheckDlgButton(h,idMu,gCfg->getLocalMute(ch)?BST_CHECKED:BST_UNCHECKED);
            if(gCfg->getLocalGainDb){
                float db=gCfg->getLocalGainDb(ch);
                int trk=dbToTrk(db);
                int cur=(int)SendDlgItemMessageW(h,ch==0?ID_CH1_GAIN_TRK:ID_CH2_GAIN_TRK,TBM_GETPOS,0,0);
                if(cur!=trk) SendDlgItemMessageW(h,ch==0?ID_CH1_GAIN_TRK:ID_CH2_GAIN_TRK,TBM_SETPOS,TRUE,trk);
                char b[32]; std::snprintf(b,sizeof(b),"Gain %.1f dB",db);
                SetWindowTextA(GetDlgItem(h,ch==0?ID_CH1_GAIN_TXT:ID_CH2_GAIN_TXT),b);
            }
        }
        auto fillList=[&](int id,std::function<std::vector<std::string>()> fn,std::function<int()> selFn){
            if(!fn) return;
            auto v=fn();
            HWND lb=GetDlgItem(h,id);
            int cnt=(int)SendMessageW(lb,LB_GETCOUNT,0,0);
            if(cnt!=(int)v.size()){
                SendMessageW(lb,LB_RESETCONTENT,0,0);
                for(auto &s: v){ std::wstring w; w.reserve(s.size()); for(char c: s) w.push_back((wchar_t)(unsigned char)c); SendMessageW(lb,LB_ADDSTRING,0,(LPARAM)w.c_str()); }
            } else {
                for(size_t i=0;i<v.size();++i){
                    wchar_t buf[256]={}; SendMessageW(lb,LB_GETTEXT,(WPARAM)i,(LPARAM)buf);
                    std::wstring w(buf); std::string cur; cur.reserve(w.size()); for(wchar_t c: w) cur.push_back((char)c);
                    if(cur!=v[i]){ SendMessageW(lb,LB_DELETESTRING,(WPARAM)i,0); std::wstring w2; w2.reserve(v[i].size()); for(char c: v[i]) w2.push_back((wchar_t)(unsigned char)c); SendMessageW(lb,LB_INSERTSTRING,(WPARAM)i,(LPARAM)w2.c_str()); }
                }
            }
            if(selFn){ int sel=selFn(); SendMessageW(lb,LB_SETCURSEL,(WPARAM)(sel>=0?sel:-1),0); }
        };
        fillList(ID_CH1_LIST,gCfg->getCh1Names,gCfg->getCh1Sel);
        fillList(ID_CH2_LIST,gCfg->getCh2Names,gCfg->getCh2Sel);
        for(int ch=0;ch<2;++ch){
            int id= ch==0?ID_CH1_WHOLE:ID_CH2_WHOLE;
            if(gCfg->getWholeBypass){
                bool b=gCfg->getWholeBypass(ch);
                SetWindowTextA(GetDlgItem(h,id), b? "Whole ON":"Whole OFF");
            }
        }
        if(gCfg->getAsioText) SetWindowTextA(GetDlgItem(h,ID_ASIO_TXT), gCfg->getAsioText().c_str());
        if(gCfg->getXrunsText) SetWindowTextA(GetDlgItem(h,ID_XRUN_TXT), gCfg->getXrunsText().c_str());
        if(gCfg->getHwMonText) SetWindowTextA(GetDlgItem(h,ID_HW_MON_TXT), gCfg->getHwMonText().c_str());
        if(gCfg->getHwHpText) SetWindowTextA(GetDlgItem(h,ID_HW_HP_TXT), gCfg->getHwHpText().c_str());
        auto syncHwSlider=[&](int trkId, std::function<std::optional<double>()> fn){
            if(!fn) return;
            auto v=fn();
            HWND trk=GetDlgItem(h,trkId);
            bool dragging = (GetCapture()==trk);
            if(dragging) return;
            if(!v.has_value()){
                EnableWindow(trk,FALSE);
                return;
            }
            EnableWindow(trk,TRUE);
            int pos=hwDbToPos(*v);
            int cur=(int)SendMessageW(trk,TBM_GETPOS,0,0);
            if(cur!=pos) SendMessageW(trk,TBM_SETPOS,TRUE,pos);
        };
        syncHwSlider(ID_HW_MON_TRK, gCfg->getHwMonDb);
        syncHwSlider(ID_HW_HP_TRK, gCfg->getHwHpDb);
        break;
    }
    case WM_HSCROLL:{
        HWND trk=(HWND)l;
        int id=GetDlgCtrlID(trk);
        int pos=(int)SendMessageW(trk,TBM_GETPOS,0,0);
        if(id==ID_CH1_GAIN_TRK || id==ID_CH2_GAIN_TRK){
            float db=trkToDb(pos);
            int ch = (id==ID_CH1_GAIN_TRK)?0:1;
            if(gCfg && gCfg->setLocalGainDb) gCfg->setLocalGainDb(ch,db);
            char b[32]; std::snprintf(b,sizeof(b),"Gain %.1f dB",db);
            SetWindowTextA(GetDlgItem(h,ch==0?ID_CH1_GAIN_TXT:ID_CH2_GAIN_TXT),b);
        } else if(id==ID_HW_MON_TRK || id==ID_HW_HP_TRK){
            double db=posToHwDb(pos);
            bool hp=(id==ID_HW_HP_TRK);
            if(gCfg && gCfg->onSetHwDb) gCfg->onSetHwDb(hp, db);
        }
        break;
    }
    case WM_COMMAND:{
        int id=LOWORD(w);
        int code=HIWORD(w);
        if(id==ID_CH1_LIST || id==ID_CH2_LIST){
            if(code==LBN_SELCHANGE){
                int ch= (id==ID_CH1_LIST)?0:1;
                int sel=(int)SendMessageW((HWND)l,LB_GETCURSEL,0,0);
                if(gCfg && gCfg->setSel) gCfg->setSel(ch,sel);
            }
            break;
        }
        if(code!=BN_CLICKED) break;
        switch(id){
        case ID_GLOBAL_MON: if(gCfg && gCfg->setGlobalMonitorOn) gCfg->setGlobalMonitorOn(IsDlgButtonChecked(h,ID_GLOBAL_MON)==BST_CHECKED); break;
        case ID_MONO: if(gCfg && gCfg->setMonoOn) gCfg->setMonoOn(IsDlgButtonChecked(h,ID_MONO)==BST_CHECKED); break;
        case ID_CH1_EN: if(gCfg && gCfg->setLocalEnable) gCfg->setLocalEnable(0, IsDlgButtonChecked(h,ID_CH1_EN)==BST_CHECKED); break;
        case ID_CH1_MUTE: if(gCfg && gCfg->setLocalMute) gCfg->setLocalMute(0, IsDlgButtonChecked(h,ID_CH1_MUTE)==BST_CHECKED); break;
        case ID_CH2_EN: if(gCfg && gCfg->setLocalEnable) gCfg->setLocalEnable(1, IsDlgButtonChecked(h,ID_CH2_EN)==BST_CHECKED); break;
        case ID_CH2_MUTE: if(gCfg && gCfg->setLocalMute) gCfg->setLocalMute(1, IsDlgButtonChecked(h,ID_CH2_MUTE)==BST_CHECKED); break;
        case ID_CH1_LOAD: if(gCfg && gCfg->onLoad) gCfg->onLoad(0); break;
        case ID_CH2_LOAD: if(gCfg && gCfg->onLoad) gCfg->onLoad(1); break;
        case ID_CH1_EDITOR: {
            int sel=-1; if(gCfg && gCfg->getCh1Sel) sel=gCfg->getCh1Sel();
            if(gCfg && gCfg->onOpenEditor) gCfg->onOpenEditor(0,sel>=0?sel:0); break;
        }
        case ID_CH2_EDITOR: {
            int sel=-1; if(gCfg && gCfg->getCh2Sel) sel=gCfg->getCh2Sel();
            if(gCfg && gCfg->onOpenEditor) gCfg->onOpenEditor(1,sel>=0?sel:0); break;
        }
        case ID_CH1_BYP: {
            int sel=-1; if(gCfg && gCfg->getCh1Sel) sel=gCfg->getCh1Sel();
            if(sel<0) sel=0;
            if(gCfg && gCfg->onBypassSel) gCfg->onBypassSel(0,sel); break;
        }
        case ID_CH2_BYP: {
            int sel=-1; if(gCfg && gCfg->getCh2Sel) sel=gCfg->getCh2Sel();
            if(sel<0) sel=0;
            if(gCfg && gCfg->onBypassSel) gCfg->onBypassSel(1,sel); break;
        }
        case ID_CH1_WHOLE: if(gCfg && gCfg->onBypassWhole) gCfg->onBypassWhole(0); break;
        case ID_CH2_WHOLE: if(gCfg && gCfg->onBypassWhole) gCfg->onBypassWhole(1); break;
        case ID_CH1_REMOVE: {
            int sel=-1; if(gCfg && gCfg->getCh1Sel) sel=gCfg->getCh1Sel();
            if(sel<0) sel=0;
            if(gCfg && gCfg->onRemove) gCfg->onRemove(0,sel); break;
        }
        case ID_CH2_REMOVE: {
            int sel=-1; if(gCfg && gCfg->getCh2Sel) sel=gCfg->getCh2Sel();
            if(sel<0) sel=0;
            if(gCfg && gCfg->onRemove) gCfg->onRemove(1,sel); break;
        }
        case ID_HW_MON_UP: if(gCfg && gCfg->onNudgeHw) gCfg->onNudgeHw(false, +1.0); break;
        case ID_HW_MON_DN: if(gCfg && gCfg->onNudgeHw) gCfg->onNudgeHw(false, -1.0); break;
        case ID_HW_HP_UP: if(gCfg && gCfg->onNudgeHw) gCfg->onNudgeHw(true, +1.0); break;
        case ID_HW_HP_DN: if(gCfg && gCfg->onNudgeHw) gCfg->onNudgeHw(true, -1.0); break;
        }
        break;
    }
    case WM_DESTROY: KillTimer(h,ID_TIMER); gHwnd=nullptr; break;
    case WM_CLOSE: ShowWindow(h,SW_HIDE); return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

HWND DiagGui_Create(HINSTANCE hInst, const DiagGuiConfig& cfg){
    static bool reg=false;
    if(!reg){
        WNDCLASSW wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=kCls;
        wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc); reg=true;
    }
    static DiagGuiConfig stored;
    stored=cfg; gCfg=&stored;
    HWND hwnd=CreateWindowExW(0,kCls,L"Audient Console - C2 Diag Controls (--gui)",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,520,420,nullptr,nullptr,hInst,nullptr);
    if(hwnd){ ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd); }
    return hwnd;
}
void DiagGui_Destroy(HWND hwnd){ if(!hwnd) hwnd=gHwnd; if(hwnd) DestroyWindow(hwnd); }
