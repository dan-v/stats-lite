#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
int main(){ CFDictionaryRef d = CGSessionCopyCurrentDictionary(); if(!d){puts("no session");return 0;}
 CFBooleanRef l = CFDictionaryGetValue(d, CFSTR("CGSSessionScreenIsLocked")); CFBooleanRef on = CFDictionaryGetValue(d, CFSTR("kCGSSessionOnConsoleKey"));
 printf("locked=%d onConsole=%d\n", l?CFBooleanGetValue(l):0, on?CFBooleanGetValue(on):-1); return 0; }
