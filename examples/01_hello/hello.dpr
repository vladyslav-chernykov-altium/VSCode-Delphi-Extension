program hello;

{$APPTYPE CONSOLE}
{$D+}              // debug info
{$L+}              // local symbols
{$Y+}              // symbol reference info
{$O-}              // optimization off, easier stepping
{$INLINE OFF}

uses
  System.SysUtils;

type
  TPoint = record
    X: Integer;
    Y: Integer;
  end;

  TColor = (clRed, clGreen, clBlue);

function Add(A, B: Integer): Integer;
var
  Tmp: Integer;
begin
  Tmp := A + B;
  Result := Tmp;
end;

var
  P: TPoint;
  C: TColor;
  S: Integer;

begin
  P.X := 3;
  P.Y := 4;
  C := clGreen;
  S := Add(P.X, P.Y);
  Writeln('Sum = ', S, ', Color = ', Ord(C));
end.
