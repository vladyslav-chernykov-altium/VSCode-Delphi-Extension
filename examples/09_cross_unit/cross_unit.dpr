program cross_unit;

// Cross-unit type-reference fixture for the cross-unit field
// resolution work.  Three supporting units (Shapes / Items /
// Layout) each declare types that the others (and this .dpr)
// consume as field types or locals.
//
// Layered design:
//   Shapes.pas  -> TPoint, TColor, TSize   (no methods)
//   Items.pas   -> TItem class             (fields from Shapes)
//   Layout.pas  -> TLayout class           (fields from Items + Shapes)
//   cross_unit  -> locals of all three     (consumer)
//
// Cross-references to verify in the resulting RSM / PDB:
//   - cross_unit's lItem (TItem) -> opaque cross-unit ref to
//     Items.TItem (TItem decl lives in Items.pas).
//   - lItem.fPos / fSize / fColor cross-unit-ref to Shapes.*
//     (TItem class header + fields are in Items.pas, but the
//     field TYPES are in Shapes.pas).
//   - cross_unit's lLayout (TLayout) -> opaque ref to
//     Layout.TLayout.
//   - lLayout.fOrigin -> Shapes.TPoint (cross-unit field type)
//   - lLayout.fLeadItem -> Items.TItem (another cross-unit field
//     type, this time pointing to a class).
// Each cross-unit reference is a potential headache for our
// type resolver -- the consuming unit's RSM only carries a 0x66
// NAME entry for the foreign type (no 0x47 header / 0x2c fields).
// Field resolution must walk the .rsm globally to find the
// declaring unit's 0x2a / 0x47 / 0x2c records.

{$APPTYPE CONSOLE}
{$D+}
{$L+}
{$Y+}
{$O-}
{$INLINE OFF}
{$W+}

uses
  System.SysUtils,
  Shapes   in 'Shapes.pas',
  Items    in 'Items.pas',
  Layout   in 'Layout.pas';

procedure SetBreakHere;
begin
  Sleep(0);
end;

procedure ProbeAll;
var
  lPoint:  TPoint;
  lSize:   TSize;
  lColor:  TColor;
  lItem:   TItem;
  lLayout: TLayout;
begin
  lPoint.X := 11;
  lPoint.Y := 22;
  lSize.Width  := 100;
  lSize.Height := 200;
  lColor := clGreen;

  lItem := TItem.Create('first', lPoint, lSize, lColor);
  lItem.Tag := 42;

  lLayout := TLayout.Create(lPoint, lItem);

  Writeln('item="', lItem.Name, '" lead="', lLayout.LeadName, '"');
  SetBreakHere;
  Writeln('done');

  lItem.Free;
  lLayout.Free;
end;

begin
  try
    ProbeAll;
  except
    on E: Exception do
      Writeln(E.ClassName, ': ', E.Message);
  end;
end.
