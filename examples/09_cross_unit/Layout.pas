unit Layout;

// Cross-unit fixture: SECOND-CONSUMER unit.  Declares TLayout
// class whose fields reference BOTH Shapes.pas (TPoint as origin)
// AND Items.pas (TItem as inner element).  cross_unit.dpr
// instantiates a TLayout local + a TItem local + a TPoint local
// so all three types get cross-unit references from the .dpr's
// perspective.

interface

uses
  Shapes, Items;

type
  TLayout = class
  private
    fOrigin:    TPoint;
    fLeadItem:  TItem;
    fItemCount: Integer;
  public
    constructor Create(const AOrigin: TPoint; ALead: TItem);
    function Origin: TPoint;
    function LeadName: string;
  end;

implementation

constructor TLayout.Create(const AOrigin: TPoint; ALead: TItem);
begin
  fOrigin    := AOrigin;
  fLeadItem  := ALead;
  fItemCount := 1;
end;

function TLayout.Origin: TPoint;
begin
  Result := fOrigin;
end;

function TLayout.LeadName: string;
begin
  if fLeadItem <> nil then
    Result := fLeadItem.Name
  else
    Result := '(no lead)';
end;

end.
