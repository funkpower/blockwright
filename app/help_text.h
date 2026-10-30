#pragma once
// The help text for the tools and the layer kinds (#153): one Markdown file
// each, bundled under :/help (app/help/tools/<tool id>.md and
// app/help/layers/<kind name>.md), so the docs site's manual can take the
// same files. A tool's first paragraph is plain text, a sentence or two on
// what the tool does: its tooltip's line. The rest says what the buttons
// and keys do and, as "- **Option**: ..." items, what each of the tool's
// settings in Tool Options changes. The Tool Options panel, the Tools
// menu's tooltips, Help > Manual and the Layers dock all read them here.

#include "blockwright/layer.h"

#include <QString>
#include <QStringList>

namespace bw::help {

// The tool's help as Markdown, or empty when there is no file for the id.
QString toolText(const QString& id);
// Its first paragraph, as plain text; empty with no file.
QString toolSummary(const QString& id);
// The ids that have a file, sorted.
QStringList toolIds();

// The layer kind's help as Markdown; empty for a kind with no file.
QString layerText(LayerKind kind);

// Markdown as a tooltip shows it: the paragraphs one to a line, "**"
// dropped.
QString plainText(const QString& markdown);

} // namespace bw::help
