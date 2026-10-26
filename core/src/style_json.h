// A text style as the project file holds it (a layer's "style", PLAN §6),
// shared by the project reader and the Library's text-style entries
// (library.h), so both read one form.
#pragma once

#include "blockwright/text_style.h"

#include <QJsonObject>
#include <QString>

#include <optional>
#include <string>
#include <string_view>

namespace bw::project {

// {"effects": [...]}: the effects in the stack's order, each its kind, its
// switch and its settings, every setting written.
QJsonObject textStyleJson(const TextStyle& style);

// The style `o` holds, checked (textStyleProblem). A key an effect doesn't
// know is skipped, as a newer minor version's is. Throws ProjectError
// naming `what`.
TextStyle textStyleFromJson(const QJsonObject& o, const std::string& what);

// A key some object in `json` holds twice, decoded, or nothing. `json` is
// valid JSON (Qt has parsed it); QJsonDocument keeps only the last of a
// repeated key, so repeats are found here, in every object at any depth.
std::optional<QString> repeatedKey(std::string_view json);

} // namespace bw::project
