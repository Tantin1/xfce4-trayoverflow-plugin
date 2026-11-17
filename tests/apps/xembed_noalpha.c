/* App antigua de prueba (bandeja clásica XEmbed) sin alfa, como Wine o Tk:
 * fondo blanco, cuadro negro con un "ojo" blanco adentro. Con "shape"
 * recorta además la ventana a un círculo aproximado (como Wine).
 * Uso: xembed_noalpha [shape]. Lo compila tests/run.sh (necesita cc). */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main (int argc, char **argv)
{
  int shaped = argc > 1 && strcmp (argv[1], "shape") == 0;
  Display *d = XOpenDisplay (NULL);
  int s = DefaultScreen (d);
  char sel_name[64];
  Atom sel, opcode, info;
  Window w, tray = None;
  XClassHint ch = { shaped ? "shaped-app" : "noalpha-app", shaped ? "shaped-app" : "noalpha-app" };
  long xinfo[2] = { 0, 1 };
  int i;

  w = XCreateSimpleWindow (d, RootWindow (d, s), 0, 0, 22, 22, 0, 0, WhitePixel (d, s));
  XSetClassHint (d, w, &ch);
  XStoreName (d, w, ch.res_name);
  info = XInternAtom (d, "_XEMBED_INFO", False);
  XChangeProperty (d, w, info, info, 32, PropModeReplace, (unsigned char *) xinfo, 2);
  XSelectInput (d, w, ExposureMask | StructureNotifyMask);

  if (shaped)
    {
      /* Un "círculo" de rectángulos: las esquinas quedan afuera */
      XRectangle r[3] = { { 6, 1, 10, 20 }, { 3, 3, 16, 16 }, { 1, 6, 20, 10 } };
      XShapeCombineRectangles (d, w, ShapeBounding, 0, 0, r, 3, ShapeSet, Unsorted);
    }

  snprintf (sel_name, sizeof sel_name, "_NET_SYSTEM_TRAY_S%d", s);
  sel = XInternAtom (d, sel_name, False);
  opcode = XInternAtom (d, "_NET_SYSTEM_TRAY_OPCODE", False);
  for (i = 0; i < 100 && (tray = XGetSelectionOwner (d, sel)) == None; i++)
    usleep (100000);
  if (tray == None)
    return 1;

  {
    XEvent ev;
    memset (&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = tray;
    ev.xclient.message_type = opcode;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = 0;   /* SYSTEM_TRAY_REQUEST_DOCK */
    ev.xclient.data.l[2] = w;
    XSendEvent (d, tray, False, NoEventMask, &ev);
    XFlush (d);
  }

  for (;;)
    {
      XEvent ev;
      XNextEvent (d, &ev);
      if (ev.type == Expose || ev.type == ConfigureNotify || ev.type == MapNotify)
        {
          XWindowAttributes wa;
          GC gc = XCreateGC (d, w, 0, NULL);
          int sz, m;
          XGetWindowAttributes (d, w, &wa);
          sz = wa.width < wa.height ? wa.width : wa.height;
          m = sz / 5;
          XSetForeground (d, gc, WhitePixel (d, s));
          XFillRectangle (d, w, gc, 0, 0, wa.width, wa.height);
          XSetForeground (d, gc, BlackPixel (d, s));
          XFillRectangle (d, w, gc, m, m, sz - 2 * m, sz - 2 * m);
          XSetForeground (d, gc, WhitePixel (d, s));   /* el ojo */
          XFillRectangle (d, w, gc, sz / 2 - m / 2 - 1, sz / 2 - m / 2 - 1, m + 2, m + 2);
          XFreeGC (d, gc);
          XFlush (d);
        }
    }
}
