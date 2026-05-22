unit App.Colors;

interface

type
  TColor = (clRed, clGreen, clBlue);

function ColorName(C: TColor): string;

implementation

function ColorName(C: TColor): string;
begin
  case C of
    clRed:   Result := 'red';
    clGreen: Result := 'green';
    clBlue:  Result := 'blue';
  else
    Result := '?';
  end;
end;

end.
