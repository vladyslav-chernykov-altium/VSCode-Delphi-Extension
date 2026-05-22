program primitives;

{$APPTYPE CONSOLE}
{$D+}
{$L+}
{$Y+}
{$O-}
{$INLINE OFF}

uses
  System.SysUtils;

// Each global is a distinct primitive type so we can study how the
// RSM encodes its type id. gI / gI2 are both Integer so we can also
// observe whether two same-typed globals share a per-unit type id.
var
  gI:  Integer;
  gI2: Integer;
  gC:  Cardinal;
  gB:  Byte;
  gW:  Word;
  gSh: ShortInt;
  gSm: SmallInt;
  gL:  Int64;
  gU:  UInt64;
  gF:  Single;
  gD:  Double;
  gO:  Boolean;
  gH:  Char;

begin
  gI  := -1;
  gI2 := -100;
  gC  := 2;
  gB  := 3;
  gW  := 4;
  gSh := -5;
  gSm := -6;
  gL  := -7;
  gU  := 8;
  gF  := 1.5;
  gD  := 2.5;
  gO  := True;
  gH  := 'X';
  Writeln('I=',  gI, ' I2=', gI2, ' C=', gC, ' B=', gB, ' W=', gW,
          ' Sh=', gSh, ' Sm=', gSm,
          ' L=', gL, ' U=', gU,
          ' F=', gF:0:2, ' D=', gD:0:2,
          ' O=', gO, ' H=', gH);
end.
