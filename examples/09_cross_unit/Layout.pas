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
    function GetOrigin: TPoint;
    function GetLeadName: string;
  public
    constructor Create(const AOrigin: TPoint; ALead: TItem);
    // Method-backed read-only -- computed, returns a record-type
    // (Shapes.TPoint).
    property Origin:    TPoint  read GetOrigin;
    // Method-backed read-only -- computed, returns a primitive.
    property LeadName:  string  read GetLeadName;
    // Field-backed read-only -- cross-unit class-pointer type
    // (Items.TItem).
    property LeadItem:  TItem   read fLeadItem;
    // Field-backed read/write -- primitive.
    property ItemCount: Integer read fItemCount write fItemCount;
  end;

implementation

constructor TLayout.Create(const AOrigin: TPoint; ALead: TItem);
begin
  fOrigin    := AOrigin;
  fLeadItem  := ALead;
  fItemCount := 1;
end;

function TLayout.GetOrigin: TPoint;
begin
  Result := fOrigin;
end;

function TLayout.GetLeadName: string;
begin
  if fLeadItem <> nil then
    Result := fLeadItem.Name
  else
    Result := '(no lead)';
end;

end.
