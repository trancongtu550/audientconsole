#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DailyGui.h"
#include "DailyTheme.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma warning(disable:4244 4189 4100 4458 4505 4457)
namespace audient::daily_gui {
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
static const wchar_t* kCls=L"AudientDailyConsole";
static DailyGuiConfig* gCfg=nullptr;
static HWND gHwnd=nullptr;
static float gUserScale=1.f;
static int gDpi=96;
static float dpiScale(){return (float)gDpi/96.f;}
static float totalScale(){return dpiScale()*gUserScale;}
static int S(int dip){return (int)std::lround((float)dip*(float)totalScale());}
static HFONT gFntTitle=nullptr,gFntSub=nullptr,gFntHdr=nullptr,gFntBtn=nullptr,gFntSmall=nullptr,gFntMono=nullptr,gFntScale=nullptr;
static void rebuildFonts(){
    if(gFntTitle)DeleteObject(gFntTitle);
    if(gFntSub)DeleteObject(gFntSub);
    if(gFntHdr)DeleteObject(gFntHdr);
    if(gFntBtn)DeleteObject(gFntBtn);
    if(gFntSmall)DeleteObject(gFntSmall);
    if(gFntMono)DeleteObject(gFntMono);
    if(gFntScale)DeleteObject(gFntScale);
    gFntTitle=createFont(S(22),true);
    gFntSub=createFont(S(11),false);
    gFntHdr=createFont(S(18),true);
    gFntBtn=createFont(S(11),true);
    gFntSmall=createFont(S(10),false);
    gFntMono=createFont(S(10),false,L"Consolas");
    gFntScale=createFont(S(9),false);
}
struct Ball{float disp=0.f,hold=0.f;std::chrono::steady_clock::time_point holdUntil{};float update(float peak){const float atk=0.50f,rel=0.16f,holdSec=0.60f;if(peak>disp)disp=disp*(1-atk)+peak*atk;else disp=disp*(1-rel)+peak*rel;auto now=std::chrono::steady_clock::now();if(peak>hold){hold=peak;holdUntil=now+std::chrono::milliseconds((int)(holdSec*1000));}else if(now>holdUntil)hold=std::max(hold*0.994f,disp);return disp;}};
static Ball bRaw[2],bPost[2];
static Theme th;
static RECT rTop{},rCh1{},rCh2{},rMaster{};
static RECT rMeterRaw[2]{},rMeterPost[2]{};
static RECT rKnob[2]{};
static RECT rBtnMute[2]{},rBtnLocal[2]{};
static RECT rAddInsert[2]{},rWholeByp[2]{};
static RECT rGlobalCard{},rMonoCard{};
static RECT rKnobMon{},rKnobHp{};
static RECT rScaleSeg[4]{};
static RECT rStatusCard{};
static RECT rScaleBox{};
struct SlotHit{RECT card{};RECT bEdit{};RECT bByp{};RECT bRem{};int idx=-1;};
static std::vector<SlotHit> gHits[2];
static POINT gMousePt{-10000,-10000};
static int gHoverKind=0,gHoverCh=-1,gHoverIdx=-1,gHoverBtn=-1;
static int gPressKind=0,gPressCh=-1,gPressIdx=-1,gPressBtn=-1;
static int gDragKnob=0;static POINT gDragStart{};static float gDragStartVal=0.f;static bool gDragging=false;
static constexpr double kHwFloor=-127.0,kHwCeil=-6.0;
static std::wstring widen(const std::string& s){std::wstring w;w.reserve(s.size());for(char c:s)w.push_back((wchar_t)(unsigned char)c);return w;}
static std::string fmtDb(float p){if(p<=0.0001f)return "-inf";char b[32];std::snprintf(b,sizeof(b),"%.1f",20.0*std::log10((double)p));return b;}
static void fillRectC(HDC dc,RECT rr,COLORREF c){HBRUSH b=CreateSolidBrush(c);FillRect(dc,&rr,b);DeleteObject(b);}
static void frameRound(HDC dc,RECT rr,int rad,COLORREF bord){HBRUSH br=(HBRUSH)GetStockObject(NULL_BRUSH);HPEN pen=CreatePen(PS_SOLID,1,bord);HBRUSH ob=(HBRUSH)SelectObject(dc,br);HPEN op=(HPEN)SelectObject(dc,pen);RoundRect(dc,rr.left,rr.top,rr.right,rr.bottom,rad,rad);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(pen);}
static void fillRound(HDC dc,RECT rr,int rad,COLORREF fill,COLORREF bord){
    HBRUSH br=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,bord);HBRUSH ob=(HBRUSH)SelectObject(dc,br);HPEN op=(HPEN)SelectObject(dc,pen);
    RoundRect(dc,rr.left,rr.top,rr.right,rr.bottom,rad,rad);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(br);DeleteObject(pen);
}
static void drawPanel(HDC dc,RECT rr,int rad){
    TRIVERTEX v[2];v[0].x=rr.left;v[0].y=rr.top;v[0].Red=(COLORREF)(GetRValue(th.panelHi)<<8);v[0].Green=(GetGValue(th.panelHi)<<8);v[0].Blue=(GetBValue(th.panelHi)<<8);v[0].Alpha=0;
    v[1].x=rr.right;v[1].y=rr.bottom;v[1].Red=(COLORREF)(GetRValue(th.panelLo)<<8);v[1].Green=(GetGValue(th.panelLo)<<8);v[1].Blue=(GetBValue(th.panelLo)<<8);v[1].Alpha=0;
    GRADIENT_RECT gr{0,1};HDC tmp=dc; (void)tmp;
    // simple solid + highlight line for now (GradientFill would need Msimg32 clipped to roundrect; keep solid for crispness)
    fillRound(dc,rr,rad,th.panel,th.border);
    // top highlight
    HPEN hp=CreatePen(PS_SOLID,1,blend(th.panelHi,RGB(255,255,255),18));HPEN op=(HPEN)SelectObject(dc,hp);
    MoveToEx(dc,rr.left+rad,rr.top,nullptr);LineTo(dc,rr.right-rad,rr.top);
    SelectObject(dc,op);DeleteObject(hp);
}
static void drawTextC(HDC dc,RECT rr,const wchar_t* txt,HFONT f,COLORREF col,UINT fmt=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS){
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,col);HFONT of=(HFONT)SelectObject(dc,f);DrawTextW(dc,txt,-1,&rr,fmt);SelectObject(dc,of);
}
static COLORREF meterCol(float db){if(db>-6.f)return th.red;if(db>-18.f)return th.yellow;return th.green;}
static void drawMeter(HDC dc,RECT rc,float peak,float hold){
    fillRectC(dc,rc,RGB(14,16,20));
    frameRound(dc,rc,S(4),th.border);
    // inner
    RECT inner{rc.left+1,rc.top+1,rc.right-1,rc.bottom-1};
    // clip fill
    float db = peak<=0.0001f ? -90.f : (float)(20.0*std::log10((double)peak));
    float norm=(db+60.f)/60.f;if(norm<0)norm=0;if(norm>1)norm=1;
    int avail=inner.bottom-inner.top;
    int fillH=(int)(avail*norm);
    RECT fr{inner.left,inner.bottom-fillH,inner.right,inner.bottom};
    if(fillH>0){
        int h=fr.bottom-fr.top;
        if(h<1)h=1;
        // three zones: green 0-60% (-60 to -18), yellow 60-90% (-18 to -6), red 90-100%
        int greenTop = inner.bottom - (int)(avail*0.60f);
        int yellowTop = inner.bottom - (int)(avail*0.90f);
        auto fillSeg=[&](RECT r,COLORREF c){HBRUSH b=CreateSolidBrush(c);FillRect(dc,&r,b);DeleteObject(b);};
        if(fr.top >= yellowTop){
            // entirely green/yellow lower
            if(fr.top >= greenTop) fillSeg(fr,th.green);
            else {RECT gl{fr.left,greenTop,fr.right,fr.bottom};RECT yl{fr.left,fr.top,fr.right,greenTop};fillSeg(gl,th.green);fillSeg(yl,th.yellow);}
        } else {
            // spans red
            RECT gl{fr.left,greenTop,fr.right,fr.bottom};fillSeg(gl,th.green);
            RECT yl{fr.left,yellowTop,fr.right,greenTop};fillSeg(yl,th.yellow);
            RECT rl{fr.left,fr.top,fr.right,yellowTop};fillSeg(rl,th.red);
            // thin separators
            HPEN sp=CreatePen(PS_SOLID,1,blend(th.bg,RGB(0,0,0),40));HPEN o=(HPEN)SelectObject(dc,sp);
            MoveToEx(dc,fr.left,yellowTop,nullptr);LineTo(dc,fr.right,yellowTop);
            MoveToEx(dc,fr.left,greenTop,nullptr);LineTo(dc,fr.right,greenTop);
            SelectObject(dc,o);DeleteObject(sp);
        }
        // subtle vertical gloss
        // gloss removed
    }
    if(hold>0.002f){
        float hdb=(float)(20.0*std::log10((double)hold));float hn=(hdb+60.f)/60.f;if(hn<0)hn=0;if(hn>1)hn=1;
        int hy=inner.bottom - (int)(avail*hn);
        HPEN hp=CreatePen(PS_SOLID,1,RGB(255,255,255));HPEN o=(HPEN)SelectObject(dc,hp);MoveToEx(dc,inner.left,hy,nullptr);LineTo(dc,inner.right,hy);SelectObject(dc,o);DeleteObject(hp);
        // glow small
        HPEN gp=CreatePen(PS_SOLID,1,blend(RGB(255,255,255),th.amber,30));o=(HPEN)SelectObject(dc,gp);MoveToEx(dc,inner.left,hy+1,nullptr);LineTo(dc,inner.right,hy+1);SelectObject(dc,o);DeleteObject(gp);
    }
}
static void drawKnob(HDC dc,RECT rc,float db,bool hw){
    int cx=(rc.left+rc.right)/2, cy=(rc.top+rc.bottom)/2, rad=(rc.right-rc.left)/2 - S(2);
    // shadow
    RECT sh{rc.left+S(2),rc.top+S(3),rc.right+S(2),rc.bottom+S(2)};HBRUSH shb=CreateSolidBrush(RGB(10,12,16));HPEN shp=CreatePen(PS_SOLID,1,RGB(10,12,16));HBRUSH ob=(HBRUSH)SelectObject(dc,shb);HPEN op=(HPEN)SelectObject(dc,shp);Ellipse(dc,sh.left,sh.top,sh.right,sh.bottom);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(shb);DeleteObject(shp);
    // outer bezel
    HBRUSH obz=CreateSolidBrush(th.panelHi);HPEN pen=CreatePen(PS_SOLID,1,th.borderHi);ob=(HBRUSH)SelectObject(dc,obz);op=(HPEN)SelectObject(dc,pen);Ellipse(dc,rc.left,rc.top,rc.right,rc.bottom);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(obz);DeleteObject(pen);
    // inner face gradient-ish: two ellipses
    int innerPad=S(7);RECT ir{rc.left+innerPad,rc.top+innerPad,rc.right-innerPad,rc.bottom-innerPad};HBRUSH ib=CreateSolidBrush(th.bg);HPEN ip=CreatePen(PS_SOLID,1,th.border);ob=(HBRUSH)SelectObject(dc,ib);op=(HPEN)SelectObject(dc,ip);Ellipse(dc,ir.left,ir.top,ir.right,ir.bottom);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(ib);DeleteObject(ip);
    // highlight top
    HPEN hl=CreatePen(PS_SOLID,1,blend(th.panelHi,RGB(255,255,255),22));op=(HPEN)SelectObject(dc,hl);Arc(dc,rc.left+S(4),rc.top+S(4),rc.right-S(4),rc.bottom-S(4), rc.left+rad, rc.top, rc.right, rc.top+rad);SelectObject(dc,op);DeleteObject(hl);
    // ticks
    for(int i=0;i<=10;++i){
        float t=(float)i/10.f;float ang=-135.f + t*270.f;float radA=ang*3.1415926f/180.f;
        int x1=cx+(int)(std::cos(radA)*(rad - S(6)));int y1=cy+(int)(std::sin(radA)*(rad - S(6)));
        int x2=cx+(int)(std::cos(radA)*(rad - S(2)));int y2=cy+(int)(std::sin(radA)*(rad - S(2)));
        bool major=(i%5==0);COLORREF tc=major? th.textDim: th.borderHi;HPEN tp=CreatePen(PS_SOLID,major?S(2):1,tc);op=(HPEN)SelectObject(dc,tp);MoveToEx(dc,x1,y1,nullptr);LineTo(dc,x2,y2);SelectObject(dc,op);DeleteObject(tp);
    }
    float norm = hw ? (float)((db - kHwFloor)/(kHwCeil - kHwFloor)) : (db+60.f)/60.f;if(norm<0)norm=0;if(norm>1)norm=1;
    float ang=-135.f + norm*270.f;float radA=ang*3.1415926f/180.f;
    // amber arc thick
    // draw arc via polyline approx
    HPEN ap=CreatePen(PS_SOLID,S(4),th.amber);op=(HPEN)SelectObject(dc,ap);
    // approx arc from -135 to ang
    int segs=28;float start=-135.f;bool started=false;for(int i=0;i<=segs;++i){float tt=(float)i/(float)segs;float a=start+tt*(ang-start);if(a<start-0.1f)continue;float rA=a*3.1415926f/180.f;int x=cx+(int)(std::cos(rA)*(rad - S(9)));int y=cy+(int)(std::sin(rA)*(rad - S(9)));if(!started){MoveToEx(dc,x,y,nullptr);started=true;}else LineTo(dc,x,y);}SelectObject(dc,op);DeleteObject(ap);
    // indicator line
    int x2=cx+(int)(std::cos(radA)*(rad - S(12)));int y2=cy+(int)(std::sin(radA)*(rad - S(12)));
    HPEN lp=CreatePen(PS_SOLID,S(2),th.amberHi);op=(HPEN)SelectObject(dc,lp);MoveToEx(dc,cx,cy,nullptr);LineTo(dc,x2,y2);SelectObject(dc,op);DeleteObject(lp);
    // center dot
    HBRUSH dot=CreateSolidBrush(th.amber);HPEN dp=CreatePen(PS_SOLID,1,th.amberLo);ob=(HBRUSH)SelectObject(dc,dot);op=(HPEN)SelectObject(dc,dp);int d=S(5);Ellipse(dc,cx-d,cy-d,cx+d,cy+d);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(dot);DeleteObject(dp);
}
static void drawButton(HDC dc,RECT rc,const wchar_t* txt,bool active,bool hover,bool pressed,COLORREF activeBg,COLORREF activeBd,COLORREF activeFg){
    COLORREF bg=th.panelHi,bd=th.border,fg=th.text;
    if(active){bg=blend(activeBg,th.panelHi,22);bd=activeBd;fg=activeFg;}
    if(pressed)bg=blend(bg,RGB(0,0,0),20);
    else if(hover)bg=blend(bg,RGB(255,255,255),10);
    fillRound(dc,rc,S(6),bg,bd);
    drawTextC(dc,rc,txt,gFntBtn,fg,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}
static void drawToggleCard(HDC dc,RECT card,const wchar_t* title,bool on,bool hover,bool pressed){
    COLORREF bg=th.panelHi;if(hover)bg=blend(bg,RGB(255,255,255),6);if(pressed)bg=blend(bg,RGB(0,0,0),12);
    fillRound(dc,card,S(7),bg,th.border);
    RECT tl{card.left+S(10),card.top,card.left+S(180),card.bottom};
    drawTextC(dc,tl,title,gFntBtn,th.textDim,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    // switch: track + thumb
    int swW=S(42),swH=S(20), swX=card.right - swW - S(10), swY=card.top + (card.bottom-card.top - swH)/2;
    RECT track{swX,swY,swX+swW,swY+swH};COLORREF trBg= on? th.amber: th.ledOff;COLORREF trBd= on? th.amberLo: th.border;
    fillRound(dc,track,swH/2,trBg,trBd);
    int thum=S(16);int tx= on? track.right - thum - S(2) : track.left + S(2);int ty=track.top + (swH - thum)/2;
    HBRUSH tb=CreateSolidBrush(RGB(245,246,248));HPEN tp=CreatePen(PS_SOLID,1,blend(trBg,RGB(0,0,0),15));HBRUSH ob=(HBRUSH)SelectObject(dc,tb);HPEN op=(HPEN)SelectObject(dc,tp);Ellipse(dc,tx,ty,tx+thum,ty+thum);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(tb);DeleteObject(tp);
    if(on){RECT onLbl{track.left+S(6),track.top,track.left+S(20),track.bottom};drawTextC(dc,onLbl,L"ON",gFntSmall,RGB(32,24,8),DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
}
static void drawSlotCard(HDC dc,RECT rc,const std::string& nameStr,bool isBypassed,bool isSelected,bool hover,bool hoverEdit,bool hoverByp,bool hoverRem,RECT &outEdit,RECT &outByp,RECT &outRem){
    COLORREF bg=th.panelHi,bd=th.border;if(isSelected){bg=th.panelHi;bd=th.amber;}else if(hover)bg=blend(bg,RGB(255,255,255),7);
    fillRound(dc,rc,S(7),bg,bd);
    // LED
    int ledR=S(4);int ledX=rc.left+S(10),ledY=rc.top + (rc.bottom-rc.top)/2;
    COLORREF ledCol= isBypassed? th.ledOff: th.green;HBRUSH lb=CreateSolidBrush(ledCol);HPEN lp=CreatePen(PS_SOLID,1,blend(ledCol,RGB(0,0,0),25));
    HBRUSH ob=(HBRUSH)SelectObject(dc,lb);HPEN op=(HPEN)SelectObject(dc,lp);Ellipse(dc,ledX-ledR,ledY-ledR,ledX+ledR,ledY+ledR);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(lb);DeleteObject(lp);
    if(!isBypassed){ // glow
        HBRUSH gb=CreateSolidBrush(blend(ledCol,RGB(255,255,255),35));RECT glow{ledX-1,ledY-1,ledX+1,ledY+1};FillRect(dc,&glow,gb);DeleteObject(gb);
    }
    std::wstring w=widen(nameStr);std::wstring vendor;size_t dash=w.find(L" - ");if(dash!=std::wstring::npos){vendor=w.substr(dash+3);w=w.substr(0,dash);}
    RECT tr{rc.left+S(22),rc.top+S(5),rc.right - S(168),rc.top+S(20)};drawTextC(dc,tr,w.c_str(),gFntBtn,th.text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if(!vendor.empty()){RECT vr{rc.left+S(22),rc.top+S(19),rc.right - S(168),rc.bottom - S(4)};COLORREF vcol=isSelected? RGB(88,68,28): th.textDim2;drawTextC(dc,vr,vendor.c_str(),gFntSmall,vcol,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_END_ELLIPSIS);}
    else if(isBypassed){RECT vr{rc.left+S(22),rc.top+S(19),rc.right - S(168),rc.bottom - S(4)};COLORREF vcol=isSelected? RGB(88,68,28): th.textDim2;drawTextC(dc,vr,L"Bypassed",gFntSmall,vcol,DT_LEFT|DT_TOP|DT_SINGLELINE);}
    int btnW=S(46),btnH=S(20),gap=S(6);int bx=rc.right - (btnW*3+gap*2) - S(8),by=rc.top + (rc.bottom-rc.top - btnH)/2;
    RECT re{bx,by,bx+btnW,by+btnH};RECT rb{bx+btnW+gap,by,bx+btnW*2+gap,by+btnH};RECT rr{bx+btnW*2+gap*2,by,bx+btnW*3+gap*2,by+btnH};
    outEdit=re;outByp=rb;outRem=rr;
    auto pill=[&](RECT r,const wchar_t* t,bool hov,COLORREF bgc,COLORREF bdc,COLORREF fgc){COLORREF b=bgc;if(hov)b=blend(b,RGB(255,255,255),12);fillRound(dc,r,S(5),b,bdc);drawTextC(dc,r,t,gFntSmall,fgc,DT_CENTER|DT_VCENTER|DT_SINGLELINE);};
    pill(re,L"Edit",hoverEdit,RGB(46,52,66),th.border, th.text);
    bool bypActive=isBypassed; // show amber when bypassed
    pill(rb, isBypassed?L"Active":L"Bypass",hoverByp, bypActive? blend(th.amber,RGB(50,46,36),28): RGB(46,52,66), bypActive? th.amber: th.border, bypActive? th.amberHi: th.textDim);
    // remove: red tint on hover
    COLORREF remBg= hoverRem? blend(th.muteRed,RGB(46,52,66),30): RGB(46,52,66);COLORREF remFg= hoverRem? RGB(255,220,220): th.textDim;
    pill(rr,L"Remove",hoverRem,remBg, hoverRem? th.muteRed: th.border, remFg);
}

static void rebuildHits(){
    gHits[0].clear();gHits[1].clear();
}

static void layout(HWND h){
    RECT cr;GetClientRect(h,&cr);int W=cr.right-cr.left,H=cr.bottom-cr.top;
    int topH=S(64),pad=S(18),gap=S(14);
    rTop={0,0,W,topH};
    int avail=W-pad*2-gap*2;
    int chW=avail*40/100;int masterW=avail - chW*2;
    if(chW<S(260))chW=S(260);if(masterW<S(220))masterW=S(220);
    // re-balance if exceeds
    int totalNeeded=chW*2+masterW+gap*2+pad*2;if(totalNeeded>W){int ex=totalNeeded-W;chW-=ex/3;masterW-=ex/3;}
    int availH=H - topH - pad*2;
    int idealH=S(620);
    int panelH=availH < idealH ? availH : idealH;
    int y0=topH+pad + (availH - panelH)/2;
    rCh1={pad,y0,pad+chW, y0+panelH};
    rCh2={rCh1.right+gap, y0, rCh1.right+gap+chW, y0+panelH};
    rMaster={rCh2.right+gap, y0, rCh2.right+gap+masterW, y0+panelH};
    if(rMaster.right> W-pad) rMaster.right=W-pad;
    // top scale box
    int segW=S(44),segH=S(24);int segGap=S(3);int boxW=segW*4+segGap*3+S(6);int boxH=segH+S(6);
    rScaleBox={W - boxW - S(16), (topH-boxH)/2, W - S(16), (topH-boxH)/2 + boxH};
    for(int i=0;i<4;++i){int x=rScaleBox.left+S(3)+i*(segW+segGap);int y=rScaleBox.top+S(3);rScaleSeg[i]={x,y,x+segW,y+segH};}
    // channel internals
    for(int ch=0;ch<2;++ch){
        RECT strip= ch==0? rCh1: rCh2;
        int x0=strip.left+S(14), w0=strip.right-strip.left - S(28);
        int hdrH=S(38);
        int meterW=S(26),meterH=S(200);
        int meterY=strip.top+hdrH+S(14);
        int meterX=x0+S(4);
        rMeterRaw[ch]={meterX,meterY,meterX+meterW,meterY+meterH};
        rMeterPost[ch]={meterX+meterW+S(22),meterY,meterX+meterW*2+S(22),meterY+meterH};
        int knobDia=S(128);
        int kx=rMeterPost[ch].right + S(36);
        int ky=meterY + S(8);
        // keep knob inside panel
        if(kx+knobDia > strip.right - S(14)) kx=strip.right - S(14) - knobDia;
        rKnob[ch]={kx,ky,kx+knobDia,ky+knobDia};
        int btnH=S(28);
        int btnY = std::max(rMeterRaw[ch].bottom, rKnob[ch].bottom) + S(22);
        int bx0=x0; int wAvail=w0;
        int bw=(wAvail - S(8))/2;
        rBtnMute[ch]={bx0,btnY,bx0+bw,btnY+btnH};
        rBtnLocal[ch]={bx0+bw+S(8),btnY,bx0+wAvail,btnY+btnH};
        // add-insert / whole bypass positions computed in paint loop per hits; reserve rects
        int insHdrY = btnY + btnH + S(12);
        rAddInsert[ch]={strip.right - S(124), insHdrY, strip.right - S(14), insHdrY + S(22)};
        rWholeByp[ch]={strip.right - S(148), insHdrY + S(26), strip.right - S(14), insHdrY + S(46)};
        // meter scale etc not needed
        // build slot hits dynamically
        std::vector<std::string> names;
        if(gCfg){names = ch==0? (gCfg->getCh1Names? gCfg->getCh1Names(): std::vector<std::string>{}): (gCfg->getCh2Names? gCfg->getCh2Names(): std::vector<std::string>{});}
        int slotTop = insHdrY + S(52);
        int slotH=S(52),slotGap=S(8);
        int maxSlots=(strip.bottom - S(10) - (int)rWholeByp[ch].bottom >0)? 8: 8;
        (void)maxSlots;
        gHits[ch].clear();
        for(size_t i=0;i<names.size() && i<8; ++i){
            int y=slotTop + (int)i*(slotH+slotGap);int y2=y+slotH;
            if(y2 > strip.bottom - S(10)) break;
            RECT card{x0, y, strip.right - S(14), y2};
            SlotHit sh;sh.card=card;sh.idx=(int)i;
            // button rects computed in draw pass; init placeholders based on card
            int bW=S(46),bH=S(20),gap2=S(6);int bx2=card.right - (bW*3+gap2*2) - S(8);int by2=card.top + (card.bottom-card.top - bH)/2;
            sh.bEdit={bx2,by2,bx2+bW,by2+bH};sh.bByp={bx2+bW+gap2,by2,bx2+bW*2+gap2,by2+bH};sh.bRem={bx2+bW*2+gap2*2,by2,bx2+bW*3+gap2*2,by2+bH};
            gHits[ch].push_back(sh);
        }
    }
    // master
    {
        RECT m=rMaster;
        int x=m.left+S(14), w=m.right-m.left - S(28);
        int hdrH=S(38);
        int y=m.top+hdrH+S(10);
        rGlobalCard={x,y,x+w,y+S(34)};y+=S(40);
        rMonoCard={x,y,x+w,y+S(34)};y+=S(48);
        int knobDia=S(110);
        // centered two knobs
        int totalKnobW=knobDia*2+S(20);
        int kx=x + (w - totalKnobW)/2; if(kx<x)kx=x;
        int ky=y+S(6);
        rKnobMon={kx,ky,kx+knobDia,ky+knobDia};
        rKnobHp={kx+knobDia+S(20),ky,kx+knobDia+S(20)+knobDia,ky+knobDia};
        int statusY=ky + knobDia + S(24);
        rStatusCard={x, statusY, x+w, statusY + S(150)};
    }
}

static bool ptIn(const RECT& r,POINT p){return p.x>=r.left&&p.x<r.right&&p.y>=r.top&&p.y<r.bottom;}

static int hitTest(POINT pt){
    gHoverKind=0;gHoverCh=-1;gHoverIdx=-1;gHoverBtn=-1;
    // scale segs
    for(int i=0;i<4;++i) if(ptIn(rScaleSeg[i],pt)){gHoverKind=10;gHoverIdx=i;return 10;}
    // top? settings icon approximated as left of scaleBox? ignore
    for(int ch=0;ch<2;++ch){
        if(ptIn(rKnob[ch],pt)){gHoverKind=1;gHoverCh=ch;return 1;}
        if(ptIn(rBtnMute[ch],pt)){gHoverKind=2;gHoverCh=ch;return 2;}
        if(ptIn(rBtnLocal[ch],pt)){gHoverKind=3;gHoverCh=ch;return 3;}
        if(ptIn(rAddInsert[ch],pt)){gHoverKind=4;gHoverCh=ch;return 4;}
        if(ptIn(rWholeByp[ch],pt)){gHoverKind=5;gHoverCh=ch;return 5;}
        for(auto &h: gHits[ch]){
            if(ptIn(h.bEdit,pt)){gHoverKind=6;gHoverCh=ch;gHoverIdx=h.idx;gHoverBtn=0;return 6;}
            if(ptIn(h.bByp,pt)){gHoverKind=6;gHoverCh=ch;gHoverIdx=h.idx;gHoverBtn=1;return 6;}
            if(ptIn(h.bRem,pt)){gHoverKind=6;gHoverCh=ch;gHoverIdx=h.idx;gHoverBtn=2;return 6;}
            if(ptIn(h.card,pt)){gHoverKind=7;gHoverCh=ch;gHoverIdx=h.idx;return 7;}
        }
    }
    if(ptIn(rGlobalCard,pt)){gHoverKind=8;gHoverCh=0;return 8;}
    if(ptIn(rMonoCard,pt)){gHoverKind=9;gHoverCh=1;return 9;}
    if(ptIn(rKnobMon,pt)){gHoverKind=11;return 11;}
    if(ptIn(rKnobHp,pt)){gHoverKind=12;return 12;}
    return 0;
}

static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{
        gDpi=(int)GetDpiForWindow(h);if(gDpi==0)gDpi=96;rebuildFonts();layout(h);SetTimer(h,9101,68,nullptr);PostMessageW(h,WM_TIMER,9101,0);return 0;}
    case WM_DPICHANGED:{
        gDpi=HIWORD(w);rebuildFonts();RECT* nr=(RECT*)l;SetWindowPos(h,nullptr,nr->left,nr->top,nr->right-nr->left,nr->bottom-nr->top,SWP_NOZORDER|SWP_NOACTIVATE);layout(h);InvalidateRect(h,nullptr,TRUE);return 0;}
    case WM_SIZE: layout(h);InvalidateRect(h,nullptr,TRUE);return 0;
    case WM_GETMINMAXINFO:{MINMAXINFO* mi=(MINMAXINFO*)l;mi->ptMinTrackSize.x=S(1180);mi->ptMinTrackSize.y=S(700);return 0;}
    case WM_SETCURSOR:{
        POINT pt;GetCursorPos(&pt);ScreenToClient(h,&pt);
        hitTest(pt);
        if(gHoverKind==1||gHoverKind==11||gHoverKind==12){SetCursor(LoadCursorW(nullptr,(LPCWSTR)IDC_HAND));return TRUE;}
        if(gHoverKind!=0){SetCursor(LoadCursorW(nullptr,(LPCWSTR)IDC_HAND));return TRUE;}
        break;
    }
    case WM_MOUSEMOVE:{
        POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};gMousePt=pt;
        if(gDragging){
            int dy=gDragStart.y - pt.y;
            if(gDragKnob>=1 && gDragKnob<=2){
                float sens=0.18f;if(GetKeyState(VK_SHIFT)&0x8000)sens=0.04f;
                float nd=gDragStartVal + dy*sens;if(nd<-60)nd=-60;if(nd>0)nd=0;
                int ch=gDragKnob-1;if(gCfg&&gCfg->setLocalGainDb)gCfg->setLocalGainDb(ch,nd);
                InvalidateRect(h,nullptr,FALSE);
            } else if(gDragKnob==3 || gDragKnob==4){
                float sens=0.35f;if(GetKeyState(VK_SHIFT)&0x8000)sens=0.08f;
                double nd=gDragStartVal + dy*sens;if(nd<kHwFloor)nd=kHwFloor;if(nd>kHwCeil)nd=kHwCeil;
                bool isHeadphones=gDragKnob==4;if(gCfg&&gCfg->onSetHwDb)gCfg->onSetHwDb(isHeadphones,nd);
                InvalidateRect(h,nullptr,FALSE);
            }
            return 0;
        }
        int prev=gHoverKind;hitTest(pt);if(prev!=gHoverKind)InvalidateRect(h,nullptr,FALSE);
        // hover highlight for slots
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:{
        POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};int hk=hitTest(pt);
        gPressKind=hk;gPressCh=gHoverCh;gPressIdx=gHoverIdx;gPressBtn=gHoverBtn;
        if(hk==1){gDragKnob=gHoverCh+1;gDragStart=pt;gDragStartVal=gCfg? gCfg->getLocalGainDb(gHoverCh): -6.f;gDragging=true;SetCapture(h);return 0;}
        if(hk==11){gDragKnob=3;gDragStart=pt;double v=gCfg&&gCfg->getHwMonDb&&gCfg->getHwMonDb()? *gCfg->getHwMonDb(): kHwFloor;gDragStartVal=(float)v;gDragging=true;SetCapture(h);return 0;}
        if(hk==12){gDragKnob=4;gDragStart=pt;double v=gCfg&&gCfg->getHwHpDb&&gCfg->getHwHpDb()? *gCfg->getHwHpDb(): kHwFloor;gDragStartVal=(float)v;gDragging=true;SetCapture(h);return 0;}
        SetCapture(h);return 0;
    }
    case WM_LBUTTONUP:{
        POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};int hk=hitTest(pt);
        bool same = (hk==gPressKind && gHoverCh==gPressCh && gHoverIdx==gPressIdx && gHoverBtn==gPressBtn);
        if(gDragging){gDragging=false;gDragKnob=0;ReleaseCapture();InvalidateRect(h,nullptr,FALSE);gPressKind=0;return 0;}
        if(!same){if(GetCapture()==h)ReleaseCapture();gPressKind=0;return 0;}
        // dispatch click
        if(hk==2 && gCfg&&gCfg->setLocalMute){bool cur=gCfg->getLocalMute? gCfg->getLocalMute(gHoverCh):false;gCfg->setLocalMute(gHoverCh,!cur);}
        else if(hk==3 && gCfg&&gCfg->setLocalEnable){bool cur=gCfg->getLocalEnable? gCfg->getLocalEnable(gHoverCh):false;gCfg->setLocalEnable(gHoverCh,!cur);}
        else if(hk==4 && gCfg&&gCfg->onLoad) gCfg->onLoad(gHoverCh);
        else if(hk==5 && gCfg&&gCfg->onBypassWhole) gCfg->onBypassWhole(gHoverCh);
        else if(hk==6){
            if(gHoverBtn==0 && gCfg&&gCfg->onOpenEditor) gCfg->onOpenEditor(gHoverCh,gHoverIdx);
            else if(gHoverBtn==1 && gCfg&&gCfg->onBypassSel) gCfg->onBypassSel(gHoverCh,gHoverIdx);
            else if(gHoverBtn==2 && gCfg&&gCfg->onRemove) gCfg->onRemove(gHoverCh,gHoverIdx);
        } else if(hk==7 && gCfg&&gCfg->setSel){gCfg->setSel(gHoverCh,gHoverIdx);}
        else if(hk==8 && gCfg&&gCfg->setGlobalMonitorOn){bool cur=gCfg->getGlobalMonitorOn? gCfg->getGlobalMonitorOn():false;gCfg->setGlobalMonitorOn(!cur);}
        else if(hk==9 && gCfg&&gCfg->setMonoOn){bool cur=gCfg->getMonoOn? gCfg->getMonoOn():false;gCfg->setMonoOn(!cur);}
        else if(hk==10){
            float ns=1.f;if(gHoverIdx==1)ns=1.25f;else if(gHoverIdx==2)ns=1.5f;else if(gHoverIdx==3)ns=2.f;
            gUserScale=ns;if(gCfg&&gCfg->onUserScaleChanged)gCfg->onUserScaleChanged(ns);
            rebuildFonts();layout(h);
        }
        if(GetCapture()==h)ReleaseCapture();
        gPressKind=0;InvalidateRect(h,nullptr,TRUE);return 0;
    }
    case WM_MOUSEWHEEL:{
        POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&pt);int delta=GET_WHEEL_DELTA_WPARAM(w);
        float step=(GetKeyState(VK_SHIFT)&0x8000)?0.5f:1.2f;float dir=delta>0?step:-step;
        if(ptIn(rKnob[0],pt)){float v=gCfg? gCfg->getLocalGainDb(0): -6.f;v+=dir;if(v<-60)v=-60;if(v>0)v=0;if(gCfg)gCfg->setLocalGainDb(0,v);InvalidateRect(h,nullptr,FALSE);return 0;}
        if(ptIn(rKnob[1],pt)){float v=gCfg? gCfg->getLocalGainDb(1): -6.f;v+=dir;if(v<-60)v=-60;if(v>0)v=0;if(gCfg)gCfg->setLocalGainDb(1,v);InvalidateRect(h,nullptr,FALSE);return 0;}
        if(ptIn(rKnobMon,pt)){double v=gCfg&&gCfg->getHwMonDb&&gCfg->getHwMonDb()? *gCfg->getHwMonDb(): kHwFloor;v+=dir;if(v<kHwFloor)v=kHwFloor;if(v>kHwCeil)v=kHwCeil;if(gCfg&&gCfg->onSetHwDb)gCfg->onSetHwDb(false,v);InvalidateRect(h,nullptr,FALSE);return 0;}
        if(ptIn(rKnobHp,pt)){double v=gCfg&&gCfg->getHwHpDb&&gCfg->getHwHpDb()? *gCfg->getHwHpDb(): kHwFloor;v+=dir;if(v<kHwFloor)v=kHwFloor;if(v>kHwCeil)v=kHwCeil;if(gCfg&&gCfg->onSetHwDb)gCfg->onSetHwDb(true,v);InvalidateRect(h,nullptr,FALSE);return 0;}
        break;
    }
    case WM_TIMER:{
        if(w!=9101)break;
        if(gCfg){
            if(gCfg->getMetersCh1){auto p=gCfg->getMetersCh1();bRaw[0].update(p.first);bPost[0].update(p.second);}
            if(gCfg->getMetersCh2){auto p=gCfg->getMetersCh2();bRaw[1].update(p.first);bPost[1].update(p.second);}
        }
        // rebuild layout if slot count changed (names size)
        // check if hits size mismatch
        for(int ch=0;ch<2;++ch){
            size_t cnt=gCfg? (ch==0? (gCfg->getCh1Names? gCfg->getCh1Names().size():0): (gCfg->getCh2Names? gCfg->getCh2Names().size():0)):0;
            if(cnt!=gHits[ch].size()){layout(h);break;}
        }
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    }
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC hdc=BeginPaint(h,&ps);
        RECT cr;GetClientRect(h,&cr);
        HDC mem=CreateCompatibleDC(hdc);HBITMAP bmp=CreateCompatibleBitmap(hdc,cr.right,cr.bottom);HBITMAP obm=(HBITMAP)SelectObject(mem,bmp);
        SetBkMode(mem,TRANSPARENT);
        fillRectC(mem,cr,th.bg);
        // top bar
        fillRectC(mem,rTop,th.panel);
        HPEN tp=CreatePen(PS_SOLID,1,th.border);HPEN op=(HPEN)SelectObject(mem,tp);MoveToEx(mem,rTop.left,rTop.bottom-1,nullptr);LineTo(mem,rTop.right,rTop.bottom-1);SelectObject(mem,op);DeleteObject(tp);
        // accent mark
        RECT acc{rTop.left+S(14),rTop.top+S(12),rTop.left+S(17),rTop.bottom-S(12)};fillRectC(mem,acc,th.amber);
        RECT tAud{rTop.left+S(24),rTop.top, rTop.left+S(168),rTop.bottom};drawTextC(mem,tAud,L"AUDIENT",gFntTitle,th.text);
        RECT tCon{rTop.left+S(168),rTop.top+S(3), rTop.left+S(238),rTop.bottom};drawTextC(mem,tCon,L"Console",gFntSub,th.textDim);
        // scale control box
        {
            bool anyHover=false;for(int i=0;i<4;++i) if(gHoverKind==10 && gHoverIdx==i) anyHover=true;
            fillRound(mem,rScaleBox,S(7),th.panelHi,th.border);
            RECT lbl{rScaleBox.left - S(42), rScaleBox.top, rScaleBox.left - S(6), rScaleBox.bottom};drawTextC(mem,lbl,L"Scale",gFntSmall,th.textDim2,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            const wchar_t* segs[4]={L"100%",L"125%",L"150%",L"200%"};
            int curSel=0;if(gUserScale>1.35f&&gUserScale<1.6f)curSel=1;else if(gUserScale>1.6f&&gUserScale<1.85f)curSel=2;else if(gUserScale>=1.85f)curSel=3;
            for(int i=0;i<4;++i){
                bool sel=i==curSel;bool hov=gHoverKind==10&&gHoverIdx==i;
                COLORREF bg= sel? th.amber: th.panel;COLORREF bd= sel? th.amberLo: th.border;COLORREF fg= sel? RGB(26,20,8): hov? th.text: th.textDim;
                if(hov && !sel) bg=blend(th.panelHi,RGB(255,255,255),8);
                fillRound(mem,rScaleSeg[i],S(5),bg,bd);
                drawTextC(mem,rScaleSeg[i],segs[i],gFntScale,fg,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            }
        }
        // center capsules: parse ASIO text and xruns text
        if(gCfg && gCfg->getAsioText){
            std::string a=gCfg->getAsioText();std::wstring aw=widen(a);
            // dot
            bool connected = a.find("Connected")!=std::string::npos || a.find("ON")!=std::string::npos || a.find("ASIO")!=std::string::npos;
            int pillH=S(24), pillPad=S(22);
            int cx=rTop.left+S(292);
            // measure widths roughly
            int wAsio=S(150);int wRate=S(96);int wXr=S(100);int wOv=S(110);
            std::string xr = gCfg->getXrunsText? gCfg->getXrunsText(): "";
            // split xruns text "Xruns 0 Overloads 0" -> two pills
            std::string xr1="Xruns 0", ov1="Overloads 0";
            size_t pos=xr.find("Over"); if(pos!=std::string::npos){xr1=xr.substr(0,pos); ov1=xr.substr(pos);}
            auto trim=[](std::string s){while(!s.empty()&&isspace((unsigned char)s.back()))s.pop_back();while(!s.empty()&&isspace((unsigned char)s.front()))s.erase(s.begin());return s;};
            xr1=trim(xr1);ov1=trim(ov1);
            std::wstring wRateS=widen(a.find("/")!=std::string::npos? a.substr(a.find("48")!=std::string::npos? a.find("48"):0): a);
            // Instead use getAsioText directly for first pill, extract rate part?
            // Simplify: first pill = ASIO, second = 48kHz /64, third = Xruns, fourth=Overloads
            int gap=S(8);int cur=cx;
            auto pill=[&](int x,int ww,const wchar_t* txt,bool withDot, COLORREF dotCol){
                RECT pr{x, rTop.top+S(12), x+ww, rTop.top+S(12)+pillH};
                fillRound(mem,pr,S(12),RGB(28,32,42),th.border);
                if(withDot){
                    int d=S(7);int dcx=x+S(10),dcy=pr.top + (pillH-d)/2;
                    // outer glow
                    HBRUSH gb=CreateSolidBrush(blend(dotCol,RGB(0,0,0),40));HPEN gp=CreatePen(PS_SOLID,1,blend(dotCol,RGB(0,0,0),20));
                    HBRUSH ob2=(HBRUSH)SelectObject(mem,gb);HPEN op2=(HPEN)SelectObject(mem,gp);Ellipse(mem,dcx-1,dcy-1,dcx+d+1,dcy+d+1);SelectObject(mem,ob2);SelectObject(mem,op2);DeleteObject(gb);DeleteObject(gp);
                    HBRUSH db=CreateSolidBrush(dotCol);HPEN dpp=CreatePen(PS_SOLID,1,dotCol);ob2=(HBRUSH)SelectObject(mem,db);op2=(HPEN)SelectObject(mem,dpp);Ellipse(mem,dcx,dcy,dcx+d,dcy+d);SelectObject(mem,ob2);SelectObject(mem,op2);DeleteObject(db);DeleteObject(dpp);
                    RECT tr{x+S(22), pr.top, x+ww-S(6), pr.bottom};drawTextC(mem,tr,txt,gFntSmall,th.textDim,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                } else {
                    RECT tr{x+S(8), pr.top, x+ww-S(6), pr.bottom};drawTextC(mem,tr,txt,gFntSmall,th.textDim,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                }
                return x+ww+gap;
            };
            std::wstring wAsioTxt=aw; if(wAsioTxt.size()>18) wAsioTxt=wAsioTxt.substr(0,18);
            cur=pill(cur,wAsio, wAsioTxt.c_str(), true, RGB(68,212,120));
            // second pill rate - extract "48 kHz / 64"
            std::wstring rateTxt=L"48 kHz / 64"; // default; if ASIO text contains "/", use that segment
            {
                size_t p1=a.find("48"); if(p1!=std::string::npos){size_t p2=a.find(" ",p1+2); size_t p3=a.find("/",p1); if(p3!=std::string::npos){std::string seg=a.substr(p1, 18); while(!seg.empty()&&seg.back()==' ')seg.pop_back(); rateTxt=widen(seg); if(rateTxt.size()>14) rateTxt=rateTxt.substr(0,14);}
                }
            }
            cur=pill(cur,wRate, rateTxt.c_str(), false, th.textDim);
            cur=pill(cur,wXr, widen(xr1).c_str(), false, th.textDim);
            cur=pill(cur,wOv, widen(ov1).c_str(), false, th.textDim);
            (void)cur;
        }
        // panels
        drawPanel(mem,rCh1,S(10));
        drawPanel(mem,rCh2,S(10));
        drawPanel(mem,rMaster,S(10));
        // channel headers
        for(int ch=0;ch<2;++ch){
            RECT strip= ch==0? rCh1: rCh2;
            // amber accent left
            RECT acc2{strip.left+S(10), strip.top+S(10), strip.left+S(13), strip.top+S(30)};fillRectC(mem,acc2,th.amber);
            RECT chLbl{strip.left+S(20), strip.top+S(10), strip.left+S(90), strip.top+S(28)};wchar_t buf[16];wsprintfW(buf,L"CH %d",ch+1);drawTextC(mem,chLbl,buf,gFntHdr,th.text);
            RECT inLbl{strip.left+S(64), strip.top+S(10), strip.left+S(150), strip.top+S(28)};wchar_t ibuf[16];wsprintfW(ibuf,L"INPUT %d",ch+1);drawTextC(mem,inLbl,ibuf,gFntSmall,th.textDim2);
            // separator
            HPEN sp=CreatePen(PS_SOLID,1,th.border);op=(HPEN)SelectObject(mem,sp);MoveToEx(mem,strip.left+S(12),strip.top+S(48),nullptr);LineTo(mem,strip.right-S(12),strip.top+S(48));SelectObject(mem,op);DeleteObject(sp);
        }
        {
            RECT rm=rMaster;RECT acc2{rm.left+S(10),rm.top+S(10),rm.left+S(13),rm.top+S(30)};fillRectC(mem,acc2,th.amber);
            RECT ml{rm.left+S(20),rm.top+S(10),rm.left+S(120),rm.top+S(28)};drawTextC(mem,ml,L"MASTER",gFntHdr,th.text);
            HPEN sp=CreatePen(PS_SOLID,1,th.border);op=(HPEN)SelectObject(mem,sp);MoveToEx(mem,rm.left+S(12),rm.top+S(52),nullptr);LineTo(mem,rm.right-S(12),rm.top+S(52));SelectObject(mem,op);DeleteObject(sp);
        }
        // meters + scales
        for(int ch=0;ch<2;++ch){
            // labels RAW/POST
            RECT lbRaw{rMeterRaw[ch].left - S(2), rMeterRaw[ch].top - S(14), rMeterRaw[ch].right+S(4), rMeterRaw[ch].top - S(2)};
            RECT lbPost{rMeterPost[ch].left - S(2), rMeterPost[ch].top - S(14), rMeterPost[ch].right+S(4), rMeterPost[ch].top - S(2)};
            drawTextC(mem,lbRaw,L"RAW",gFntSmall,th.textDim2,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            drawTextC(mem,lbPost,L"POST",gFntSmall,th.textDim2,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            float dRaw=bRaw[ch].disp, dPost=bPost[ch].disp;
            drawMeter(mem,rMeterRaw[ch],dRaw,bRaw[ch].hold);
            drawMeter(mem,rMeterPost[ch],dPost,bPost[ch].hold);
            // dB scales to right of POST meter
            for(int db=-60; db<=0; db+=6){
                float norm=(db+60)/60.f;int y=rMeterRaw[ch].bottom - (int)((rMeterRaw[ch].bottom - rMeterRaw[ch].top)*norm);
                bool major=(db==0||db==-6||db==-12||db==-18||db==-24||db==-36||db==-48||db==-60);
                HPEN pen=CreatePen(PS_SOLID,1, major? th.textDim2: th.borderHi);op=(HPEN)SelectObject(mem,pen);
                int x1=rMeterPost[ch].right+S(4);int x2=x1 + (major? S(6): S(3));
                MoveToEx(mem,x1,y,nullptr);LineTo(mem,x2,y);SelectObject(mem,op);DeleteObject(pen);
                if(major){
                    wchar_t b[8];wsprintfW(b,L"%d",db);
                    RECT tr{rMeterPost[ch].right+S(12), y-S(7), rMeterPost[ch].right+S(30), y+S(7)};
                    drawTextC(mem,tr,b,gFntSmall,th.textDim2,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                }
            }
            // numeric pills under meters
            auto pillVal=[&](RECT rc,float peak){
                std::string s=fmtDb(peak);std::wstring w=widen(s+ " dB");
                int pillW=S(44);int pillH=S(14);int cx=(rc.left+rc.right)/2; RECT pill{cx-pillW/2, rc.bottom+S(3), cx+pillW/2, rc.bottom+S(3)+pillH};
                // determine color by level
                float db= peak<=0.0001f? -90.f: (float)(20.0*std::log10((double)peak));
                COLORREF c= th.textDim; if(db>-6) c=th.red; else if(db>-18) c=th.yellow;
                fillRound(mem,pill,S(4),RGB(22,26,34),th.border);
                drawTextC(mem,pill,w.c_str(),gFntMono,c,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            };
            pillVal(rMeterRaw[ch], dRaw);
            pillVal(rMeterPost[ch], dPost);
        }
        // knobs + labels
        for(int ch=0;ch<2;++ch){
            float v=gCfg&&gCfg->getLocalGainDb? gCfg->getLocalGainDb(ch): -6.f;
            drawKnob(mem,rKnob[ch],v,false);
            RECT lMon{rKnob[ch].left - S(8), rKnob[ch].top - S(16), rKnob[ch].right+S(8), rKnob[ch].top - S(4)};
            drawTextC(mem,lMon,L"MON GAIN",gFntSmall,th.textDim2,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            wchar_t kb[32];swprintf(kb,32,L"%.1f dB",v);
            RECT pill{rKnob[ch].left - S(4), rKnob[ch].bottom+S(4), rKnob[ch].right+S(4), rKnob[ch].bottom+S(20)};
            fillRound(mem,pill,S(5),RGB(22,26,34),th.border);
            drawTextC(mem,pill,kb,gFntMono,th.amber,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            // mute/local buttons
            bool muteOn=gCfg&&gCfg->getLocalMute? gCfg->getLocalMute(ch): false;
            bool locOn=gCfg&&gCfg->getLocalEnable? gCfg->getLocalEnable(ch): false;
            bool hovMute=gHoverKind==2&&gHoverCh==ch;bool hovLoc=gHoverKind==3&&gHoverCh==ch;
            bool prMute=gPressKind==2&&gPressCh==ch;bool prLoc=gPressKind==3&&gPressCh==ch;
            drawButton(mem,rBtnMute[ch],L"Mute",muteOn,hovMute,prMute, th.muteRed,th.muteRed,RGB(255,230,230));
            drawButton(mem,rBtnLocal[ch],L"Local Monitor",locOn,hovLoc,prLoc, th.amber, th.amberLo, RGB(42,28,8));
        }
        // inserts per channel
        for(int ch=0;ch<2;++ch){
            RECT strip= ch==0? rCh1: rCh2;
            RECT insLbl{strip.left+S(14), rAddInsert[ch].top, strip.left+S(100), rAddInsert[ch].bottom};
            drawTextC(mem,insLbl,L"INSERTS",gFntSmall,th.textDim2,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            // Add button
            bool hovAdd=gHoverKind==4&&gHoverCh==ch;bool prAdd=gPressKind==4&&gPressCh==ch;
            COLORREF addBg= hovAdd? blend(th.amber, th.panelHi,18): th.panelHi; if(prAdd) addBg=blend(addBg,RGB(0,0,0),12);
            fillRound(mem,rAddInsert[ch],S(5),addBg, th.amberLo);
            drawTextC(mem,rAddInsert[ch],L"+  Add Insert",gFntSmall, hovAdd? th.amberHi: th.amber,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            // slots
            std::vector<std::string> names=gCfg? (ch==0? (gCfg->getCh1Names? gCfg->getCh1Names(): std::vector<std::string>{}): (gCfg->getCh2Names? gCfg->getCh2Names(): std::vector<std::string>{})) : std::vector<std::string>{};
            int sel=gCfg? (ch==0? (gCfg->getCh1Sel? gCfg->getCh1Sel(): -1): (gCfg->getCh2Sel? gCfg->getCh2Sel(): -1)): -1;
            bool wholeByp=gCfg&&gCfg->getWholeBypass? gCfg->getWholeBypass(ch): false;
            for(size_t i=0;i<gHits[ch].size();++i){
                auto &hit=gHits[ch][i];
                bool isByp=wholeByp; // per-slot bypass not exposed; whole bypass dims all
                bool isSel=(int)i==sel;
                bool hovCard=gHoverKind==7&&gHoverCh==ch&&gHoverIdx==(int)i;
                bool hovE=gHoverKind==6&&gHoverCh==ch&&gHoverIdx==(int)i&&gHoverBtn==0;
                bool hovB=gHoverKind==6&&gHoverCh==ch&&gHoverIdx==(int)i&&gHoverBtn==1;
                bool hovR=gHoverKind==6&&gHoverCh==ch&&gHoverIdx==(int)i&&gHoverBtn==2;
                RECT oE,oB,oR;
                std::string nm=i<names.size()? names[i]: "";
                drawSlotCard(mem,hit.card,nm,isByp,isSel,hovCard,hovE,hovB,hovR,oE,oB,oR);
                // update hit rects for next hitTest (in case layout rounding changed)
                gHits[ch][i].bEdit=oE;gHits[ch][i].bByp=oB;gHits[ch][i].bRem=oR;
            }
            if(names.empty()){
                RECT empty{strip.left+S(14), rAddInsert[ch].bottom+S(10), strip.right-S(14), rAddInsert[ch].bottom+S(36)};
                fillRound(mem,empty,S(6),RGB(26,30,40),th.border);
                drawTextC(mem,empty,L"No inserts — click \"+ Add Insert\" to load a VST3",gFntSmall,th.textDim2,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            }
            // whole bypass footer toggle
            bool hovW=gHoverKind==5&&gHoverCh==ch;bool prW=gPressKind==5&&gPressCh==ch;
            COLORREF wbg= wholeByp? blend(th.amber, th.panelHi,18): th.panelHi; if(hovW) wbg=blend(wbg,RGB(255,255,255),7); if(prW) wbg=blend(wbg,RGB(0,0,0),10);
            fillRound(mem,rWholeByp[ch],S(6), wbg, wholeByp? th.amberLo: th.border);
            const wchar_t* wtxt= wholeByp? L"Whole Bypass  •  ON": L"Whole Bypass  •  OFF";
            drawTextC(mem,rWholeByp[ch],wtxt,gFntSmall, wholeByp? th.amberHi: th.textDim, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            // small dot
            int d=S(6);int dcx=rWholeByp[ch].left+S(10),dcy=rWholeByp[ch].top + (rWholeByp[ch].bottom - rWholeByp[ch].top)/2;
            HBRUSH db=CreateSolidBrush(wholeByp? th.amber: th.ledOff);HPEN dp=CreatePen(PS_SOLID,1, wholeByp? th.amberLo: th.border);HBRUSH ob2=(HBRUSH)SelectObject(mem,db);HPEN op2=(HPEN)SelectObject(mem,dp);Ellipse(mem,dcx-d,dcy-d,dcx+d,dcy+d);SelectObject(mem,ob2);SelectObject(mem,op2);DeleteObject(db);DeleteObject(dp);
        }
        // master toggles + knobs + status
        {
            bool gOn=gCfg&&gCfg->getGlobalMonitorOn? gCfg->getGlobalMonitorOn(): false;
            bool mOn=gCfg&&gCfg->getMonoOn? gCfg->getMonoOn(): false;
            bool hovG=gHoverKind==8, hovM=gHoverKind==9;bool prG=gPressKind==8, prM=gPressKind==9;
            drawToggleCard(mem,rGlobalCard,L"Global Local Monitor",gOn,hovG,prG);
            drawToggleCard(mem,rMonoCard,L"Mono \x03A3",mOn,hovM,prM);
            // hw knobs
            double hvMon=gCfg&&gCfg->getHwMonDb&&gCfg->getHwMonDb()? *gCfg->getHwMonDb(): kHwFloor;
            double hvHp=gCfg&&gCfg->getHwHpDb&&gCfg->getHwHpDb()? *gCfg->getHwHpDb(): kHwFloor;
            drawKnob(mem,rKnobMon,(float)hvMon,true);
            drawKnob(mem,rKnobHp,(float)hvHp,true);
            RECT lMon{rKnobMon.left - S(6), rKnobMon.top - S(16), rKnobMon.right+S(6), rKnobMon.top - S(4)};drawTextC(mem,lMon,L"MON HW",gFntSmall,th.textDim2,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            RECT lHp{rKnobHp.left - S(6), rKnobHp.top - S(16), rKnobHp.right+S(6), rKnobHp.top - S(4)};drawTextC(mem,lHp,L"HP HW",gFntSmall,th.textDim2,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            wchar_t b1[32],b2[32];swprintf(b1,32,L"%.1f dB",hvMon);swprintf(b2,32,L"%.1f dB",hvHp);
            RECT p1{rKnobMon.left - S(6), rKnobMon.bottom+S(6), rKnobMon.right+S(6), rKnobMon.bottom+S(22)};fillRound(mem,p1,S(5),RGB(22,26,34),th.border);drawTextC(mem,p1,b1,gFntMono,th.amber,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            RECT p2{rKnobHp.left - S(6), rKnobHp.bottom+S(6), rKnobHp.right+S(6), rKnobHp.bottom+S(22)};fillRound(mem,p2,S(5),RGB(22,26,34),th.border);drawTextC(mem,p2,b2,gFntMono,th.amber,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            // status card
            fillRound(mem,rStatusCard,S(7),RGB(28,32,42),th.border);
            RECT st{rStatusCard.left+S(10),rStatusCard.top+S(6),rStatusCard.right-S(10),rStatusCard.top+S(20)};drawTextC(mem,st,L"STATUS",gFntSmall,th.textDim2);
            HPEN sp2=CreatePen(PS_SOLID,1,th.border);op=(HPEN)SelectObject(mem,sp2);MoveToEx(mem,rStatusCard.left+S(10),rStatusCard.top+S(22),nullptr);LineTo(mem,rStatusCard.right-S(10),rStatusCard.top+S(22));SelectObject(mem,op);DeleteObject(sp2);
            std::string aTxt=gCfg&&gCfg->getAsioText? gCfg->getAsioText(): std::string("ASIO --");
            std::string xTxt=gCfg&&gCfg->getXrunsText? gCfg->getXrunsText(): std::string("Xruns 0  Overloads 0");
            bool asioOk = aTxt.find("Connected")!=std::string::npos || aTxt.find("CONNECTED")!=std::string::npos;
            std::string sRate="48 kHz", sBuf="64", sXr="0", sOvl="0";
            { size_t slash=aTxt.find('/'); if(slash!=std::string::npos){ std::string after=aTxt.substr(slash+1); std::string num; for(char c: after) if(isdigit((unsigned char)c)) num.push_back(c); else if(!num.empty()) break; if(!num.empty()) sBuf=num; } }
            { std::string low=xTxt; for(char &c: low) c=(char)tolower((unsigned char)c); size_t px=low.find("xruns"); if(px!=std::string::npos){ std::string num; for(size_t i=px+5;i<low.size();++i){ if(isdigit((unsigned char)low[i])) num.push_back(low[i]); else if(!num.empty()) break; } if(!num.empty()) sXr=num; } size_t po=low.find("overloads"); if(po!=std::string::npos){ std::string num; for(size_t i=po+9;i<low.size();++i){ if(isdigit((unsigned char)low[i])) num.push_back(low[i]); else if(!num.empty()) break; } if(!num.empty()) sOvl=num; } else { size_t po2=low.find("ovl"); if(po2!=std::string::npos){ std::string num; for(size_t i=po2+3;i<low.size();++i){ if(isdigit((unsigned char)low[i])) num.push_back(low[i]); else if(!num.empty()) break; } if(!num.empty()) sOvl=num; } } }
            int ly=rStatusCard.top+S(28);int lh=S(16);int colL=rStatusCard.left+S(10), colR=rStatusCard.right-S(10);
            auto kv=[&](const wchar_t* k, const wchar_t* v, COLORREF vc){
                RECT rk{colL,ly,colL+S(110),ly+lh}; RECT rv{colR - S(90),ly,colR,ly+lh};
                drawTextC(mem,rk,k,gFntSmall,th.textDim2,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                drawTextC(mem,rv,v,gFntSmall,vc,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
                ly+=lh+S(1);
            };
            kv(L"ASIO", asioOk? L"CONNECTED \u25cf": L"--", asioOk? th.green: th.textDim);
            kv(L"SAMPLE RATE", widen(sRate).c_str(), th.text);
            kv(L"BUFFER", widen(sBuf).c_str(), th.text);
            kv(L"XRUNS", widen(sXr).c_str(), th.text);
            kv(L"OVERLOADS", widen(sOvl).c_str(), sOvl!=std::string("0")? th.yellow: th.text);
        }
        BitBlt(hdc,0,0,cr.right,cr.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,obm);DeleteObject(bmp);DeleteDC(mem);
        EndPaint(h,&ps);return 0;
    }
    case WM_DESTROY: KillTimer(h,9101);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
HWND Create(HINSTANCE hInst,const DailyGuiConfig& cfg){
    if(gHwnd) return gHwnd;
    static DailyGuiConfig stored;stored=cfg;gCfg=&stored;gUserScale=cfg.userScale>0? cfg.userScale:1.f;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES};InitCommonControlsEx(&icc);
    WNDCLASSW wc{};wc.hInstance=hInst;wc.lpszClassName=kCls;wc.lpfnWndProc=WndProc;wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);wc.style=CS_HREDRAW|CS_VREDRAW;
    RegisterClassW(&wc);
    gDpi=96;rebuildFonts();
    int w=S(1360),hh=S(780);
    HWND hwnd=CreateWindowExW(0,kCls,L"Audient Console",WS_OVERLAPPEDWINDOW|WS_CLIPSIBLINGS|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,w,hh,nullptr,nullptr,hInst,nullptr);
    if(!hwnd) return nullptr;
    gHwnd=hwnd;ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);return hwnd;
}
void Destroy(HWND hwnd){if(!hwnd)hwnd=gHwnd;if(hwnd)DestroyWindow(hwnd);gHwnd=nullptr;gCfg=nullptr;if(gFntTitle){DeleteObject(gFntTitle);gFntTitle=nullptr;}if(gFntSub){DeleteObject(gFntSub);gFntSub=nullptr;}if(gFntHdr){DeleteObject(gFntHdr);gFntHdr=nullptr;}if(gFntBtn){DeleteObject(gFntBtn);gFntBtn=nullptr;}if(gFntSmall){DeleteObject(gFntSmall);gFntSmall=nullptr;}if(gFntMono){DeleteObject(gFntMono);gFntMono=nullptr;}if(gFntScale){DeleteObject(gFntScale);gFntScale=nullptr;}}
}
