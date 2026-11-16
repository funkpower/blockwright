A Generator layer holds a program's cached output as glyph cells with foreground and background colours.

Change parameters and Run below; painting tools cannot edit the output.

The cached cells composite like a Cells layer and export in the visible composite. Opening or exporting does not run the generator.

File > Open Scene... opens a Python scene as one Generator layer per scene layer, after asking to run it.

Layer > Add Generator Layer... adds one generator from a directory holding a generator.json, above the active layer, after asking to run it. One Undo removes it. Its next run asks again unless the project's generators are already trusted.
