/* App de Windows con ícono en la bandeja, para probar el proxy con Wine.
 * Anota cada clic que recibe en el archivo que se le pasa (ruta de
 * Windows, p. ej. Z:\tmp\log.txt). Con clic derecho abre un menú, como
 * Pageant, y anota MENU-ABIERTO y MENU-CERRADO <opción> (0: ninguna). La compila tests/run.sh con
 * x86_64-w64-mingw32-gcc (si está; si no, la prueba se saltea). */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#define WM_TRAY (WM_APP + 1)
static FILE *log_file;

static LRESULT CALLBACK proc (HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  if (msg == WM_TRAY)
    {
      const char *name = NULL;
      switch (LOWORD (lp))
        {
        case WM_LBUTTONDOWN: name = "LDOWN"; break;
        case WM_LBUTTONUP: name = "LUP"; break;
        case WM_RBUTTONDOWN: name = "RDOWN"; break;
        case WM_RBUTTONUP: name = "RUP"; break;
        case WM_LBUTTONDBLCLK: name = "LDBL"; break;
        }
      if (name != NULL)
        {
          POINT p;
          GetCursorPos (&p);
          fprintf (log_file, "%s %ld %ld\n", name, p.x, p.y);
          fflush (log_file);
        }
      if (LOWORD (lp) == WM_RBUTTONUP)
        {
          /* Menú como el de cualquier app de Windows (Pageant, etc.) */
          HMENU menu = CreatePopupMenu ();
          POINT p;
          int cmd;
          AppendMenuA (menu, MF_STRING, 1, "Uno");
          AppendMenuA (menu, MF_STRING, 2, "Dos");
          GetCursorPos (&p);
          SetForegroundWindow (w);
          fprintf (log_file, "MENU-ABIERTO\n");
          fflush (log_file);
          cmd = TrackPopupMenu (menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, w, NULL);
          PostMessageA (w, WM_NULL, 0, 0);
          fprintf (log_file, "MENU-CERRADO %d\n", cmd);
          fflush (log_file);
          DestroyMenu (menu);
        }
      return 0;
    }
  return DefWindowProcA (w, msg, wp, lp);
}

int WINAPI WinMain (HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
  WNDCLASSA wc = { 0 };
  NOTIFYICONDATAA nid = { 0 };
  HWND w;
  MSG m;

  log_file = fopen (cmd && *cmd ? cmd : "trayapp.log", "a");
  wc.lpfnWndProc = proc;
  wc.hInstance = inst;
  wc.lpszClassName = "trayapp";
  RegisterClassA (&wc);
  w = CreateWindowA ("trayapp", "trayapp", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);

  nid.cbSize = sizeof nid;
  nid.hWnd = w;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = LoadIcon (NULL, IDI_INFORMATION);
  strcpy (nid.szTip, "trayapp");
  Shell_NotifyIconA (NIM_ADD, &nid);
  fprintf (log_file, "LISTA\n");
  fflush (log_file);

  while (GetMessageA (&m, NULL, 0, 0) > 0)
    {
      TranslateMessage (&m);
      DispatchMessageA (&m);
    }
  return 0;
}
