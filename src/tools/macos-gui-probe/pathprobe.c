#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_n; static unsigned char g_types[8192];
static void apply(void* i, const CGPathElement* e){ (void)i; if(g_n<8192) g_types[g_n++]=(unsigned char)e->type; }

static int check(const char* what, CGPathRef m)
{
    const unsigned char* b = (const unsigned char*)m;
    unsigned long long kind, np, ne, base, buf;
    memcpy(&kind,b+0x10,8); memcpy(&np,b+0x18,8); memcpy(&ne,b+0x20,8);
    memcpy(&base,b+0x28,8); memcpy(&buf,b+0x30,8);
    g_n=0; CGPathApply(m, NULL, apply);
    if (kind != 9) { printf("%-28s kind=%llu (closed form)\n", what, kind); return 1; }
    const unsigned char* p = (const unsigned char*)buf;
    int ok = ((unsigned long long)g_n == ne);
    size_t pts = 0;
    for (int i = 0; i < g_n; ++i) {
        if (p[base - 1 - (size_t)i] != g_types[i]) ok = 0;
        pts += g_types[i]==0||g_types[i]==1 ? 1 : g_types[i]==2 ? 2 : g_types[i]==3 ? 3 : 1;
    }
    printf("%-28s kind=9 pts=%llu(calc %zu) els=%llu(apply %d) base=0x%llx  types %s\n",
           what, np, pts, ne, g_n, base, ok ? "MATCH" : "MISMATCH");
    return ok;
}

int main(void) {
    int all = 1;
    CGMutablePathRef a = CGPathCreateMutable();
    CGPathMoveToPoint(a,NULL,1,2); CGPathAddLineToPoint(a,NULL,3,4);
    CGPathAddQuadCurveToPoint(a,NULL,5,6,7,8); CGPathAddCurveToPoint(a,NULL,9,10,11,12,13,14);
    CGPathCloseSubpath(a); all &= check("move,line,quad,curve,close", a);

    CGMutablePathRef c = CGPathCreateMutable();
    CGPathMoveToPoint(c,NULL,0,0);
    for(int i=0;i<3;++i) CGPathAddCurveToPoint(c,NULL,i,i,i+1,i+1,i+2,i+2);
    CGPathCloseSubpath(c); all &= check("move+3curves+close", c);

    CGMutablePathRef d = CGPathCreateMutable();
    for (int s=0;s<5;++s){ CGPathMoveToPoint(d,NULL,s,s);
        for(int i=0;i<40;++i) CGPathAddLineToPoint(d,NULL,i,s*i); CGPathCloseSubpath(d);}
    all &= check("5 subpaths x 40 lines", d);

    all &= check("rect (closed form)", CGPathCreateWithRect(CGRectMake(0,0,10,10), NULL));
    all &= check("ellipse (closed form)", CGPathCreateWithEllipseInRect(CGRectMake(0,0,10,10), NULL));

    CTFontRef f = CTFontCreateWithName(CFSTR("Helvetica"), 48, NULL);
    const char* glyphs[] = {"divide","multiply","minus","plus","equal"};
    UniChar chars[] = {0x00F7, 0x00D7, 0x2212, 0x002B, 0x003D};
    for (int i=0;i<5;++i){ CGGlyph g; CTFontGetGlyphsForCharacters(f,&chars[i],&g,1);
        CGPathRef gp = CTFontCreatePathForGlyph(f,g,NULL);
        if (gp) all &= check(glyphs[i], gp); }
    printf("\nALL %s\n", all ? "MATCH" : "FAILED");
    return all?0:1;
}
