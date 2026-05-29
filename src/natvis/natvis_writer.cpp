#include "natvis/natvis_writer.h"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

namespace rsm2pdb::natvis {

namespace {

std::string xmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:  out += c; break;
    }
  }
  return out;
}

bool shouldEmitMethodAsGetter(const rsm2pdb::pdb::AggregateMethod& m) {
  if (m.returns_void) return false;
  if (!m.params.empty()) return false;
  if (m.name == "Create") return false;
  return true;
}

// Tier 2 heuristic: for a Pascal property named `PropName` declared
// inside class `cls`, return the expression NatVis should use to
// surface it in Watch -- the backing field expression if one matches
// the `f<PropName>` convention, otherwise the parameterless getter
// `Get<PropName>()` if the class has a matching method, otherwise
// empty (caller skips emission).
std::string resolvePropertyExpr(const rsm2pdb::pdb::AggregateRecord& cls,
                                const std::string& prop_name) {
  // Field-backed: `property X read fX [write fX]`.
  const std::string field_name = "f" + prop_name;
  for (const auto& f : cls.fields) {
    if (f.name == field_name) return field_name;
  }
  // Method-backed getter: `property X read GetX`.
  const std::string getter_name = "Get" + prop_name;
  for (const auto& m : cls.methods) {
    if (m.name == getter_name) return getter_name + "()";
  }
  // No conventional accessor; skip (Tier 2.1 will resolve via
  // accessor markers when this proves limiting).
  return std::string{};
}

} // namespace

std::string buildNatVisXml(const rsm2pdb::pdb::PdbInputs& inputs) {
  std::ostringstream f;
  std::size_t classes_emitted = 0;

  f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    << "<AutoVisualizer xmlns=\""
       "http://schemas.microsoft.com/vstudio/debugger/natvis/2010\">\n";

  for (const auto& a : inputs.aggregates) {
    if (a.kind != rsm2pdb::pdb::AggregateKind::Class) continue;

    f << "  <Type Name=\"" << xmlEscape(a.name) << "\">\n";
    f << "    <DisplayString>" << xmlEscape(a.name)
      << "</DisplayString>\n";
    f << "    <Expand>\n";

    if (a.base && *a.base < inputs.aggregates.size()) {
      const auto& base = inputs.aggregates[*a.base];
      f << "      <ExpandedItem>(" << xmlEscape(base.name)
        << "*)this</ExpandedItem>\n";
    }
    for (const auto& fld : a.fields) {
      f << "      <Item Name=\"" << xmlEscape(fld.name) << "\">"
        << xmlEscape(fld.name) << "</Item>\n";
    }
    for (const auto& m : a.methods) {
      if (!shouldEmitMethodAsGetter(m)) continue;
      f << "      <Item Name=\"" << xmlEscape(m.name) << "()\">"
        << xmlEscape(m.name) << "()</Item>\n";
    }
    // Tier 2: Pascal `property` declarations. Routed through the
    // naming heuristic in resolvePropertyExpr (field-backed `fX` or
    // method-backed `GetX()`). Property name in the Watch UI keeps
    // its Pascal spelling (no `f` / `Get` prefix); the expression
    // accessed is whichever convention exists on this class.
    for (const auto& prop_name : a.property_names) {
      const std::string expr = resolvePropertyExpr(a, prop_name);
      if (expr.empty()) continue;
      f << "      <Item Name=\"" << xmlEscape(prop_name) << "\">"
        << xmlEscape(expr) << "</Item>\n";
    }

    f << "    </Expand>\n";
    f << "  </Type>\n";
    ++classes_emitted;
  }

  f << "</AutoVisualizer>\n";

  // Caller treats empty string == "no classes, skip embedding".
  if (classes_emitted == 0) return std::string{};
  return f.str();
}

bool writeNatVisToFile(const std::string& path,
                       const rsm2pdb::pdb::PdbInputs& inputs,
                       std::string& error_out) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    error_out = "could not open natvis file for writing: " + path;
    return false;
  }
  const std::string xml = buildNatVisXml(inputs);
  f << xml;
  if (!f) {
    error_out = "natvis write failed mid-stream: " + path;
    return false;
  }
  return true;
}

} // namespace rsm2pdb::natvis
