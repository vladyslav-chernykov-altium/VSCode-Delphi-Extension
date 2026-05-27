program two_units;

{$APPTYPE CONSOLE}
{$D+}
{$L+}
{$Y+}
{$O-}
{$INLINE OFF}

uses
  System.SysUtils,
  Geometry      in 'Geometry.pas',
  App.Colors    in 'App.Colors.pas';

var
  P1, P2: Geometry.TPoint;
  C:      App.Colors.TColor;
  D:      Double;
  S:      Integer;

begin
  P1.X := 0;
  P1.Y := 0;
  P2.X := 3;
  P2.Y := 4;
  S := Geometry.Add(P1.X, P2.Y);
  D := Geometry.DistanceSq(P1, P2);
  C := App.Colors.clGreen;
  Writeln('S=', S, ' D=', D, ' C=', App.Colors.ColorName(C));
end.
