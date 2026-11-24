/*
 * paint - blank canvas template for a Plan 9 paint program.
 *
 * Structure follows rd-thread.c: libthread, one threadmain, and the
 * mouse / keyboard / resize channels multiplexed with alt().
 * Builds with plan9port and on native Plan 9 / 9front.
 *
 *	mouse b1	draw with the current tool (a freehand pen)
 *	mouse b3	menu
 *	keys		q or DEL: quit   c: clear   1-5: colour   + -: pen size
 *
 * Extension points:
 *	Tool	add an entry to tools[] (line, rect, flood fill, ...).
 *		down/drag/up receive canvas coordinates.
 *	canvas	offscreen document; its size only ever grows, so
 *		shrinking and re-growing the window loses nothing.
 *	menu	add an item to the menu0 list and a case in domenu().
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <keyboard.h>
#include <mouse.h>

#define	MAXPEN	20
#define	MAX(a, b)	((a) > (b) ? (a) : (b))

typedef struct Tool Tool;
struct Tool
{
	char	*name;
	void	(*down)(Point);		/* button pressed at p */
	void	(*drag)(Point, Point);	/* moved from p0 to p1, button held */
	void	(*up)(Point);		/* button released at p */
};

enum
{
	Mclear,
	Mblack,
	Mred,
	Mgreen,
	Mblue,
	Mthin,
	Mthick,
	Mquit,
};

static char *menu0[] = {
	"clear", "black", "red", "green", "blue", "thin", "thick", "quit", nil
};
static Menu menu = { menu0 };

static ulong palette[] = { DBlack, DRed, DGreen, DBlue, DYellow };

static Mousectl	*mc;
static Keyboardctl	*kc;
static Image	*canvas;	/* the picture; origin at (0,0) */
static Image	*ink;		/* 1x1 replicated source in the pen colour */
static int	penr = 1;	/* pen radius; line width is 2*penr+1 */

static void	pendown(Point);
static void	pendrag(Point, Point);
static void	penup(Point);

static Tool tools[] = {
	{ "pen", pendown, pendrag, penup },
};
static Tool *tool = &tools[0];

/* Copy a rectangle of the canvas (canvas coordinates) to the window. */
static void
blit(Rectangle r)
{
	draw(screen, rectaddpt(r, screen->r.min), canvas, nil, r.min);
}

static void
redraw(void)
{
	blit(canvas->r);
	flushimage(display, 1);
}

/* Make sure the canvas covers at least need, keeping its old contents. */
static void
ensurecanvas(Rectangle need)
{
	Rectangle r;
	Image *n;

	if(canvas != nil && Dx(canvas->r) >= Dx(need) && Dy(canvas->r) >= Dy(need))
		return;
	r = Rect(0, 0, Dx(need), Dy(need));
	if(canvas != nil)
		r = Rect(0, 0, MAX(Dx(r), Dx(canvas->r)), MAX(Dy(r), Dy(canvas->r)));
	n = allocimage(display, r, screen->chan, 0, DWhite);
	if(n == nil)
		sysfatal("allocimage: %r");
	if(canvas != nil){
		draw(n, canvas->r, canvas, nil, canvas->r.min);
		freeimage(canvas);
	}
	canvas = n;
}

static void
setcolor(ulong rgba)
{
	Image *c;

	c = allocimage(display, Rect(0, 0, 1, 1), screen->chan, 0, rgba);
	if(c == nil)
		sysfatal("allocimage: %r");
	draw(ink, ink->r, c, nil, ZP);
	freeimage(c);
}

static void
clearcanvas(void)
{
	draw(canvas, canvas->r, display->white, nil, ZP);
	redraw();
}

static void
setpen(int r)
{
	if(r < 0)
		r = 0;
	if(r > MAXPEN)
		r = MAXPEN;
	penr = r;
}

/* The window changed size (or was uncovered): rebuild and repaint. */
static void
resize(void)
{
	if(getwindow(display, Refnone) < 0)
		sysfatal("getwindow: %r");
	ensurecanvas(Rect(0, 0, Dx(screen->r), Dy(screen->r)));
	redraw();
}

/* Freehand pen. */
static void
pendown(Point p)
{
	pendrag(p, p);		/* a click leaves a dot */
}

static void
pendrag(Point a, Point b)
{
	Rectangle r;

	line(canvas, a, b, Enddisc, Enddisc, penr, ink, ZP);
	r = insetrect(canonrect(Rpt(a, b)), -(penr+2));
	blit(r);
	flushimage(display, 1);
}

static void
penup(Point)
{
}

static void
domenu(int hit)
{
	switch(hit){
	case Mclear:	clearcanvas(); break;
	case Mblack:	setcolor(palette[0]); break;
	case Mred:	setcolor(palette[1]); break;
	case Mgreen:	setcolor(palette[2]); break;
	case Mblue:	setcolor(palette[3]); break;
	case Mthin:	setpen(1); break;
	case Mthick:	setpen(6); break;
	case Mquit:	threadexitsall(nil);
	}
}

static void
dokey(Rune r)
{
	switch(r){
	case 'q':
	case Kdel:
		threadexitsall(nil);
	case 'c':
		clearcanvas();
		break;
	case '+':
		setpen(penr+1);
		break;
	case '-':
		setpen(penr-1);
		break;
	default:
		if(r >= '1' && r < '1'+nelem(palette))
			setcolor(palette[r-'1']);
		break;
	}
}

static void
domouse(Mouse m)
{
	static int drawing;
	static Point last;
	Point p;
	int hit;

	p = subpt(m.xy, screen->r.min);		/* window -> canvas coordinates */

	if(!drawing && (m.buttons&4)){
		hit = menuhit(3, mc, &menu, nil);
		if(hit >= 0)
			domenu(hit);
		return;
	}
	if(m.buttons&1){
		if(!drawing){
			drawing = 1;
			last = p;
			tool->down(p);
		}else if(!eqpt(p, last)){
			tool->drag(last, p);
			last = p;
		}
	}else if(drawing){
		drawing = 0;
		if(!eqpt(p, last))
			tool->drag(last, p);	/* release may carry a final move */
		tool->up(p);
	}
}

static void
usage(void)
{
	fprint(2, "usage: paint\n");
	threadexitsall("usage");
}

void
threadmain(int argc, char *argv[])
{
	enum { AMouse, AResize, AKey, NALT };
	Alt alts[NALT+1];
	Mouse m;
	Rune r;
	int dummy;

	ARGBEGIN{
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	if(initdraw(nil, nil, "paint") < 0)
		sysfatal("initdraw: %r");
	if((mc = initmouse(nil, screen)) == nil)
		sysfatal("initmouse: %r");
	if((kc = initkeyboard(nil)) == nil)
		sysfatal("initkeyboard: %r");

	ink = allocimage(display, Rect(0, 0, 1, 1), screen->chan, 1, DBlack);
	if(ink == nil)
		sysfatal("allocimage: %r");

	resize();

	alts[AMouse].c = mc->c;
	alts[AMouse].v = &m;
	alts[AMouse].op = CHANRCV;
	alts[AResize].c = mc->resizec;
	alts[AResize].v = &dummy;
	alts[AResize].op = CHANRCV;
	alts[AKey].c = kc->c;
	alts[AKey].v = &r;
	alts[AKey].op = CHANRCV;
	alts[NALT].op = CHANEND;

	for(;;){
		switch(alt(alts)){
		case AMouse:
			domouse(m);
			break;
		case AResize:
			resize();
			break;
		case AKey:
			dokey(r);
			break;
		}
	}
}
