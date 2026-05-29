unit Shapes;

// Cross-unit fixture: PRIMITIVES unit.  Declares small record +
// enum types that Items.pas and Layout.pas reference as field
// types and cross_unit.dpr references as locals.  No methods --
// keeps this unit a pure type-declaration source so we can
// observe how the consuming units (Items / Layout / cross_unit)
// reference these types in RSM.

interface

type
  TPoint = record
    X: Integer;
    Y: Integer;
  end;

  TColor = (clRed, clGreen, clBlue, clYellow);

  TSize = record
    Width:  Integer;
    Height: Integer;
  end;

implementation

end.
