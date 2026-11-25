# obs-browser panel API header

`browser-panel.hpp` is copied unchanged from the obs-browser plugin
(https://github.com/obsproject/obs-browser, `panel/browser-panel.hpp`), which is part of OBS Studio and
licensed under the GNU General Public License, version 2 (or later), like HimitsuKao.

It is only the interface used to ask the obs-browser module for a browser widget (`QCef`, `QCefWidget`) at runtime:
the implementation stays in OBS's own obs-browser plugin, nothing from it is linked into HimitsuKao. The copy is
needed because the OBS source archive that the build downloads does not include the obs-browser submodule.
