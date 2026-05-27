program types;

// Systematic coverage of Delphi primitive + string types so we can
// reverse-engineer the RSM type-marker / trailer_type_id encoding.
//
// Strategy:
//   - Each global has a UNIQUE primitive type. Two-Integer globals
//     gI / gI2 let us see whether same type shares marker / id.
//   - Inside ProbeLocals every local is the same primitive set,
//     declared in the SAME order. That lets us cross-reference
//     "global encoding" vs "local encoding" for each type.
//   - Strings are the most interesting because Delphi has FIVE
//     distinct string types in modern versions; we exercise them
//     individually.

{$APPTYPE CONSOLE}
{$D+}
{$L+}
{$Y+}
{$O-}
{$INLINE OFF}
{$W+}

uses
  System.SysUtils;

// ---------- Global variables (one of each primitive type) ----------

var
  // integers (signed / unsigned, 1/2/4/8 bytes)
  gB:  Byte;
  gW:  Word;
  gC:  Cardinal;
  gU:  UInt64;
  gSh: ShortInt;
  gSm: SmallInt;
  gI:  Integer;
  gI2: Integer;       // second Integer -- shared-marker test
  gL:  Int64;
  // floats
  gF:  Single;
  gD:  Double;
  gE:  Extended;
  // booleans
  gO:  Boolean;
  gBB: ByteBool;
  gWB: WordBool;
  gLB: LongBool;
  // characters
  gCh: Char;          // = WideChar in modern Delphi
  gAc: AnsiChar;
  // pointers / addresses
  gP:  Pointer;
  gPI: PInteger;
  // strings (the interesting ones)
  gS:  string;        // = UnicodeString
  gAS: AnsiString;
  gWS: WideString;
  gShS: ShortString;
  gUS: UTF8String;
  gPC: PChar;
  gPAC: PAnsiChar;
  gPWC: PWideChar;

// ---------- Function with locals of each primitive type ----------

procedure ProbeLocals;
var
  lB:  Byte;
  lW:  Word;
  lC:  Cardinal;
  lU:  UInt64;
  lSh: ShortInt;
  lSm: SmallInt;
  lI:  Integer;
  lI2: Integer;
  lL:  Int64;
  lF:  Single;
  lD:  Double;
  lE:  Extended;
  lO:  Boolean;
  lBB: ByteBool;
  lWB: WordBool;
  lLB: LongBool;
  lCh: Char;
  lAc: AnsiChar;
  lP:  Pointer;
  lPI: PInteger;
  lS:  string;
  lAS: AnsiString;
  lWS: WideString;
  lShS: ShortString;
  lUS: UTF8String;
  lPC: PChar;
  lPAC: PAnsiChar;
  lPWC: PWideChar;
begin
  lB := 1; lW := 2; lC := 3; lU := 4;
  lSh := -5; lSm := -6; lI := -7; lI2 := -8; lL := -9;
  lF := 1.5; lD := 2.5; lE := 3.5;
  lO := True; lBB := True; lWB := True; lLB := True;
  lCh := 'Z'; lAc := 'A';
  lP := @lI; lPI := @lI;
  lS := 'unicode'; lAS := 'ansi'; lWS := 'wide';
  lShS := 'short'; lUS := 'utf8';
  lPC := PChar(lS); lPAC := PAnsiChar(lAS); lPWC := PWideChar(lWS);
  Writeln('locals ok ', lB, ' ', lW, ' ', lC, ' ', lU,
          ' ', lSh, ' ', lSm, ' ', lI, ' ', lI2, ' ', lL,
          ' ', lF:0:2, ' ', lD:0:2, ' ', lE:0:2,
          ' ', lO, ' ', lBB, ' ', lWB, ' ', lLB,
          ' ', lCh, ' ', lAc,
          ' ', NativeUInt(lP), ' ', NativeUInt(lPI),
          ' ', lS, ' ', lAS, ' ', lWS, ' ', lShS, ' ', lUS);
end;

// ---------- Function with parameters of each primitive type ----------

procedure ProbeParams(
  pB:  Byte;        pW:  Word;        pC:  Cardinal;    pU:  UInt64;
  pSh: ShortInt;    pSm: SmallInt;    pI:  Integer;     pL:  Int64;
  pF:  Single;      pD:  Double;      pE:  Extended;
  pO:  Boolean;     pBB: ByteBool;    pWB: WordBool;    pLB: LongBool;
  pCh: Char;        pAc: AnsiChar;
  pP:  Pointer;
  pS:  string;      pAS: AnsiString;  pWS: WideString;
  pShS: ShortString; pUS: UTF8String;
  pPC: PChar);
begin
  Writeln('params ok ', pB, ' ', pI, ' ', pF:0:2, ' ', pD:0:2,
          ' ', pO, ' ', pCh, ' ', pS, ' ', pAS, ' ', pWS);
end;

// ---------- Driver ----------

begin
  gB := 1; gW := 2; gC := 3; gU := 4;
  gSh := -5; gSm := -6; gI := -7; gI2 := -8; gL := -9;
  gF := 1.5; gD := 2.5; gE := 3.5;
  gO := True; gBB := True; gWB := True; gLB := True;
  gCh := 'Z'; gAc := 'A';
  gP := @gI; gPI := @gI;
  gS := 'unicode'; gAS := 'ansi'; gWS := 'wide';
  gShS := 'short'; gUS := 'utf8';
  gPC := PChar(gS); gPAC := PAnsiChar(gAS); gPWC := PWideChar(gWS);

  ProbeLocals;
  ProbeParams(1, 2, 3, 4, -5, -6, -7, -9, 1.5, 2.5, 3.5,
              True, True, True, True, 'Z', 'A', @gI,
              'us', 'as', 'ws', 'shs', 'utf8s', PChar(gS));

  Writeln('globals ok ', gB, ' ', gI, ' ', gI2, ' ', gF:0:2,
          ' ', gO, ' ', gCh, ' ', gS, ' ', gAS, ' ', gWS);
end.
