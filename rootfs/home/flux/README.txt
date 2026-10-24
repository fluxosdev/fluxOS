Welcome to fluxOS 0.1.1
=======================

fluxOS is an experimental hobby operating system, built entirely with
Claude. Run it in a virtual machine, and do not keep anything you care about
on it. The root filesystem lives in RAM: files you create in your home folder
are lost on reboot unless you copy them to a disk (see "Disks" below).


The desktop
-----------
  Start menu       the Start button, the Windows key or Ctrl+Esc
  Alt+Tab          switch windows;  Alt+F4 closes one
  Windows          drag the title bar to move, any edge or corner to resize,
                   double-click the title bar to maximize

  Terminal         a shell (fsh) in a window; open as many as you like
  File Manager     folders, disks and pictures; double-click to open things
  Text Editor      this file is open in it
  Web Browser      plain HTTP pages (no HTTPS yet): try 68k.news or wiby.me
  Calculator       exact decimal arithmetic
  3D Gears         the classic OpenGL demo (arrow keys or drag to turn it,
                   Space pauses, S switches between hardware and software)
  Volume Control   click the speaker in the taskbar
  Display Properties (Start menu)
                   wallpaper (Fill, Stretch, Center, Tile), video mode,
                   monitor and refresh rate


Pictures and sound
------------------
  JPEG and PNG pictures open in an image viewer, and can be your wallpaper:
  put them in /usr/share/wallpapers or your home folder and they appear in
  Display Properties > Background.

  WAV and VOC files play when you double-click them. In a terminal:
    play /usr/share/sounds/ding.wav     the Sound Blaster 16
    play -t 440                         a test tone
    beep -m "T160 L8 CDEGEC4"           the PC speaker, playing a tune
    mixer; mixer master 80              show and set the volumes


Disks
-----
  CDs and floppy disks are mounted by themselves when you put them in, on
  /mnt/cdrom and /mnt/fd0, and unmounted when you take them out. A disk you
  unmount yourself (umount /mnt/fd0) stays unmounted until it is removed.
  Hard disks are on /mnt/hda, /mnt/hdb... (lsblk lists them).

    ls /mnt/cdrom           what is on the disc
    cp notes.txt /mnt/fd0   copy to the floppy;  sync   writes it out now
    dmesg | grep mounted    what the automounter did

  Unmount a floppy (umount /mnt/fd0) before pulling it out: changes that
  have not been written yet are lost with it.

  To install fluxOS on a hard disk, double-click "Install fluxOS" on the
  desktop (only shown when started from the CD).


Multitasking
------------
  spin A 5 &         a CPU-bound task in the background (never yields)
  spin B 5 &         ...and another; the shell stays responsive
  jobs               list background jobs, 'fg %1' brings one back
  top                live CPU usage per process (^C to quit)
  ps                 the process table


The shell (fsh) is zsh-flavoured
--------------------------------
  * syntax highlighting: commands turn green when they exist
  * grey autosuggestions from history; Right arrow or ^E accepts
  * Tab completion with zsh-style listings
  * Up/Down search history by the prefix you typed
  * AUTO_CD: type a directory name to cd into it
  * globs that match nothing are an error ("no matches found")
  * continuation prompts: if> for> dquote> pipe> ...

  for i in $(seq 1 3); do echo "round $i"; done
  if [ -d /bin ]; then echo yes; else echo no; fi
  greet() { echo "hello, ${1:-world}"; }; greet flux
  echo $((6 * 7))
  ./demo.sh                     a longer scripting demo
  ls /bin | grep -c .           pipes and redirection
  sort < /etc/fshrc | uniq | head -3


Network
-------
  ifconfig; ping -c 3 10.0.2.2     the network card and its gateway
  host example.com                 DNS
  wget -O - http://example.com/    HTTP
  nc -l 7777 &  echo hi | nc localhost 7777


DOS programs
------------
  ./HELLO.COM        .COM and .EXE files run in a terminal (also: dos hello)
                     with sound: PC speaker, AdLib and Sound Blaster 16
  ls /mnt/fd0        the floppy disk in drive A: (DOS programs see it as A:)
  dos -l /tmp/dos.log GAME.EXE   log what the emulator cannot do
  Ctrl+\             stops a DOS program that will not stop


System
------
  free, uname -a, uptime, date, dmesg, fetch, reboot, poweroff

More about fluxOS is in README.md in the source tree, and the credits are
there too: the default wallpaper is a photograph by Stefan Stefancik on
Unsplash.
