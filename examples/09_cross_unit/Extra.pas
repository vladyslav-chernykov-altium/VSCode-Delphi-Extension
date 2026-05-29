unit Extra;

// Cross-unit fixture: 5th unit, added for a DIFFERENTIAL RSM
// experiment on the instance-id -> type bridge (cross-unit class-
// as-local typing).  TExtra has a deliberately distinctive field
// name (fMagicTag) so the new records are trivial to spot in a hex
// diff of cross_unit.rsm before/after this unit was added.

interface

type
  TExtra = class
  private
    fMagicTag: Integer;
    fLabel:    string;
  public
    constructor Create(ATag: Integer; const ALabel: string);
    property MagicTag: Integer read fMagicTag write fMagicTag;
    property Lbl:      string  read fLabel;
  end;

implementation

constructor TExtra.Create(ATag: Integer; const ALabel: string);
begin
  fMagicTag := ATag;
  fLabel    := ALabel;
end;

end.
