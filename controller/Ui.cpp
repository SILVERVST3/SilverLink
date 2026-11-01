// SilverLink Controller - user interface (silver theme, navy text)
// Copyright (C) 2026 Jorge Prata Alves Junior - GPLv3
#include "Ui.h"
#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace slink::ui {

namespace {
float g_scale = 1.f;
ImFont* g_fontBold = nullptr;

const ImU32 kNavy      = IM_COL32(27, 42, 74, 255);
const ImU32 kGreen     = IM_COL32(46, 158, 91, 255);
const ImU32 kAmber     = IM_COL32(214, 165, 40, 255);
const ImU32 kOrange    = IM_COL32(222, 110, 40, 255);
const ImU32 kRed       = IM_COL32(200, 55, 55, 255);
const ImU32 kGroupA    = IM_COL32(236, 239, 243, 255);
const ImU32 kGroupB    = IM_COL32(222, 227, 234, 255);
const ImU32 kHeaderA   = IM_COL32(200, 206, 215, 255);
const ImU32 kHeaderB   = IM_COL32(186, 193, 204, 255);
const ImU32 kHover     = IM_COL32(120, 150, 200, 90);
const ImU32 kMeterBg   = IM_COL32(170, 176, 186, 255);

std::set<uint64_t> g_txCollapsed, g_rxCollapsed;
uint64_t g_cfgDevice = 0;

const char* clockStateText(uint8_t s) {
    switch (s) { case CLK_LOCKED: return "Travado"; case CLK_LOCKING: return "Sincronizando"; default: return "Livre"; }
}
ImU32 statusColor(uint8_t st) {
    switch (st) { case RS_OK: return kGreen; case RS_NO_AUDIO: return kAmber; case RS_RATE_MISMATCH: return kOrange; default: return kRed; }
}
const char* statusText(uint8_t st) {
    switch (st) {
    case RS_OK: return "OK - recebendo áudio";
    case RS_NO_AUDIO: return "Sem áudio (a origem não está transmitindo: ASIO parado?)";
    case RS_RATE_MISMATCH: return "Taxa de amostragem diferente";
    default: return "Origem não encontrada na rede";
    }
}
bool online(const DeviceView& d) { return int64_t(GetTickCount64()) - d.lastSeen < 2500; }

void meterBar(ImDrawList* dl, ImVec2 a, ImVec2 b, uint8_t m, bool vertical) {
    dl->AddRectFilled(a, b, kMeterBg);
    if (m == 0) return;
    const float db = meterToDb(m);
    const float f = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
    const ImU32 c = db > -3.f ? kRed : db > -12.f ? kAmber : kGreen;
    if (vertical) dl->AddRectFilled(ImVec2(a.x, b.y - (b.y - a.y) * f), b, c);
    else          dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * f, b.y), c);
}

void sendU8(Discovery& disc, const DeviceView& d, uint8_t cmd, uint8_t v) { Writer w; w.u8(v); disc.sendControl(d, cmd, w.buf); }
void sendU32(Discovery& disc, const DeviceView& d, uint8_t cmd, uint32_t v) { Writer w; w.u32(v); disc.sendControl(d, cmd, w.buf); }

// ===========================================================================
// Routing matrix
// ===========================================================================
struct Col { int dev; int ch; };

void drawMatrix(Discovery& disc, const std::vector<DeviceView>& devs) {
    std::vector<Col> cols;
    for (int i = 0; i < (int)devs.size(); ++i) {
        if (g_txCollapsed.count(devs[i].id)) cols.push_back({i, -1});
        else for (int c = 0; c < devs[i].ann.txChannels; ++c) cols.push_back({i, c});
    }
    if (cols.size() > 500) cols.resize(500);

    const float cw = 22.f * g_scale, firstW = 210.f * g_scale;
    const float rowH = ImGui::GetTextLineHeight() + 6.f * g_scale;
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 1.6f;
    ImGuiTableFlags fl = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner |
                         ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadInnerX |
                         ImGuiTableFlags_NoPadOuterX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (!ImGui::BeginTable("##matrix", 1 + (int)cols.size(), fl, ImVec2(0, -footer))) { ImGui::PopStyleVar(2); return; }
    ImGui::TableSetupScrollFreeze(1, 2);
    ImGui::TableSetupColumn("##rx", ImGuiTableColumnFlags_WidthFixed, firstW);
    for (size_t k = 0; k < cols.size(); ++k) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, cw);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float clipLeft = ImGui::GetWindowPos().x + firstW;
    const float clipRight = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();

    // ---- header row 0: transmitter device names (click = expand/collapse)
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, rowH);
    ImGui::TableSetColumnIndex(0);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kHeaderB);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6 * g_scale);
    ImGui::TextColored(ImVec4(0.11f, 0.17f, 0.32f, 1), "Transmissores  →");
    for (size_t k = 0; k < cols.size(); ++k) {
        ImGui::TableSetColumnIndex(1 + (int)k);
        const Col& c = cols[k];
        const DeviceView& d = devs[c.dev];
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, (c.dev & 1) ? kHeaderB : kHeaderA);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(int(k));
        if (ImGui::InvisibleButton("##txh", ImVec2(cw, rowH))) {
            if (g_txCollapsed.count(d.id)) g_txCollapsed.erase(d.id); else g_txCollapsed.insert(d.id);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)\nClique para %s", d.ann.name.c_str(), ipToString(d.ip).c_str(),
                                                      c.ch < 0 ? "expandir" : "recolher");
        ImGui::PopID();
        if (k == 0 || cols[k - 1].dev != c.dev) {
            std::string label = std::string(c.ch < 0 ? "+ " : "- ") + d.ann.name;
            dl->PushClipRect(ImVec2(std::max(p.x, clipLeft), p.y), ImVec2(clipRight, p.y + rowH), false);
            if (g_fontBold) ImGui::PushFont(g_fontBold);
            dl->AddText(ImVec2(p.x + 4 * g_scale, p.y + 3 * g_scale), online(d) ? kNavy : IM_COL32(120, 120, 130, 255), label.c_str());
            if (g_fontBold) ImGui::PopFont();
            dl->PopClipRect();
        }
    }

    // ---- header row 1: transmitter channel numbers + level
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, rowH * 1.4f);
    ImGui::TableSetColumnIndex(0);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kHeaderA);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6 * g_scale);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4 * g_scale);
    ImGui::TextColored(ImVec4(0.11f, 0.17f, 0.32f, 1), "Receptores  ↓");
    for (size_t k = 0; k < cols.size(); ++k) {
        ImGui::TableSetColumnIndex(1 + (int)k);
        const Col& c = cols[k];
        const DeviceView& d = devs[c.dev];
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, (c.dev & 1) ? kHeaderB : kHeaderA);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(int(10000 + k));
        ImGui::InvisibleButton("##txc", ImVec2(cw, rowH * 1.4f));
        if (ImGui::IsItemHovered()) {
            if (c.ch >= 0) ImGui::SetTooltip("%s @ %s", d.txName(c.ch).c_str(), d.ann.name.c_str());
            else ImGui::SetTooltip("%s: %d canais (recolhido)", d.ann.name.c_str(), d.ann.txChannels);
        }
        ImGui::PopID();
        char num[8];
        if (c.ch >= 0) snprintf(num, sizeof(num), "%d", c.ch + 1); else snprintf(num, sizeof(num), "%d", d.ann.txChannels);
        ImVec2 ts = ImGui::CalcTextSize(num);
        const float tx = p.x + (cw - ts.x) * 0.5f;
        dl->AddText(ImVec2(tx, p.y + 2 * g_scale), c.ch >= 0 ? kNavy : IM_COL32(90, 100, 120, 255), num);
        if (c.ch >= 0) {
            uint8_t m = (c.ch < (int)d.txMeters.size()) ? d.txMeters[size_t(c.ch)] : 0;
            meterBar(dl, ImVec2(p.x + 3 * g_scale, p.y + rowH * 1.4f - 6 * g_scale), ImVec2(p.x + cw - 3 * g_scale, p.y + rowH * 1.4f - 2 * g_scale), m, false);
        }
    }

    // ---- body
    static uint64_t ctxRx = 0, ctxTx = 0;
    for (int ri = 0; ri < (int)devs.size(); ++ri) {
        const DeviceView& rxd = devs[ri];
        const bool rxColl = g_rxCollapsed.count(rxd.id) > 0;
        const int nRows = rxColl ? 0 : rxd.ann.rxChannels;
        for (int r = -1; r < nRows; ++r) {
            ImGui::TableNextRow(0, rowH);
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(int(ri * 1000 + r + 1));
            const ImU32 groupBg = (ri & 1) ? kGroupB : kGroupA;
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, r < 0 ? ((ri & 1) ? kHeaderB : kHeaderA) : groupBg);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (r < 0) {
                if (ImGui::InvisibleButton("##rxh", ImVec2(firstW, rowH))) {
                    if (rxColl) g_rxCollapsed.erase(rxd.id); else g_rxCollapsed.insert(rxd.id);
                }
                std::string label = std::string(rxColl ? "+ " : "- ") + rxd.ann.name;
                if (g_fontBold) ImGui::PushFont(g_fontBold);
                dl->AddText(ImVec2(p.x + 6 * g_scale, p.y + 3 * g_scale), online(rxd) ? kNavy : IM_COL32(120, 120, 130, 255), label.c_str());
                if (g_fontBold) ImGui::PopFont();
            } else {
                ImGui::InvisibleButton("##rxn", ImVec2(firstW, rowH));
                if (ImGui::IsItemHovered()) {
                    const RouteStatusEntry* rt = rxd.routeFor(r);
                    if (rt) ImGui::SetTooltip("%s  <-  %s\n%s", rxd.rxName(r).c_str(), rt->route.src.c_str(), statusText(rt->status));
                }
                char lbl[64]; snprintf(lbl, sizeof(lbl), "%2d  %s", r + 1, rxd.rxName(r).c_str());
                dl->AddText(ImVec2(p.x + 18 * g_scale, p.y + 3 * g_scale), kNavy, lbl);
                uint8_t m = (r < (int)rxd.rxMeters.size()) ? rxd.rxMeters[size_t(r)] : 0;
                meterBar(dl, ImVec2(p.x + firstW - 40 * g_scale, p.y + rowH * 0.35f), ImVec2(p.x + firstW - 6 * g_scale, p.y + rowH * 0.65f), m, false);
            }
            ImGui::PopID();

            for (size_t k = 0; k < cols.size(); ++k) {
                ImGui::TableSetColumnIndex(1 + (int)k);
                const Col& c = cols[k];
                const DeviceView& txd = devs[c.dev];
                if (r < 0 && !rxColl) {    // header row of an expanded receiver: just a band
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, (ri & 1) ? kHeaderB : kHeaderA);
                    continue;
                }
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ((c.dev + ri) & 1) ? kGroupB : kGroupA);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::PushID(int(k * 4096 + ri * 64 + r + 1 + 100000));
                bool clicked = ImGui::InvisibleButton("##cell", ImVec2(cw, rowH));
                bool hovered = ImGui::IsItemHovered();
                bool rclick  = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                ImGui::PopID();
                const ImVec2 a(cp.x + 3 * g_scale, cp.y + 3 * g_scale), b(cp.x + cw - 3 * g_scale, cp.y + rowH - 3 * g_scale);
                if (hovered) dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + rowH), kHover);

                if (r >= 0 && c.ch >= 0) {
                    const RouteStatusEntry* rt = rxd.routeFor(r);
                    const bool routed = rt && rt->route.src == txd.ann.name && rt->route.txCh == c.ch;
                    if (routed) dl->AddRectFilled(a, b, statusColor(rt->status), 3 * g_scale);
                    if (hovered) {
                        ImGui::SetTooltip("%s @ %s  ->  %s @ %s\n%s", txd.txName(c.ch).c_str(), txd.ann.name.c_str(),
                                          rxd.rxName(r).c_str(), rxd.ann.name.c_str(),
                                          routed ? statusText(rt->status) : (rt ? "Clique para substituir a rota atual" : "Clique para rotear"));
                    }
                    if (clicked) {
                        if (routed) disc.clearRoute(rxd, r);
                        else disc.setRoute(rxd, r, txd.ann.name, c.ch);
                    }
                } else {
                    int count = 0;
                    for (auto& e : rxd.routes) {
                        if (e.route.src != txd.ann.name) continue;
                        if (r >= 0 && e.route.rxCh != r) continue;
                        if (c.ch >= 0 && e.route.txCh != c.ch) continue;
                        ++count;
                    }
                    if (count > 0) {
                        const ImVec2 ctr((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                        dl->AddCircleFilled(ctr, (b.y - a.y) * 0.32f, kGreen);
                    }
                    if (hovered) ImGui::SetTooltip("%d rota(s)\nClique: expandir   |   Botão direito: opções", count);
                    if (clicked) {
                        if (c.ch < 0) g_txCollapsed.erase(txd.id);
                        if (r < 0) g_rxCollapsed.erase(rxd.id);
                    }
                    if (rclick) { ctxRx = rxd.id; ctxTx = txd.id; ImGui::OpenPopup("##mctx"); }
                }
            }
        }
    }

    if (ImGui::BeginPopup("##mctx")) {
        const DeviceView* rxd = nullptr; const DeviceView* txd = nullptr;
        for (auto& d : devs) { if (d.id == ctxRx) rxd = &d; if (d.id == ctxTx) txd = &d; }
        if (rxd && txd) {
            ImGui::TextDisabled("%s  ->  %s", txd->ann.name.c_str(), rxd->ann.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Rotear 1:1 (canal N → canal N)")) {
                const int n = std::min<int>(rxd->ann.rxChannels, txd->ann.txChannels);
                for (int i = 0; i < n; ++i) disc.setRoute(*rxd, i, txd->ann.name, i);
            }
            if (ImGui::MenuItem("Remover rotas deste transmissor")) {
                for (auto& e : rxd->routes) if (e.route.src == txd->ann.name) disc.clearRoute(*rxd, e.route.rxCh);
            }
            if (ImGui::MenuItem("Expandir ambos")) { g_rxCollapsed.erase(rxd->id); g_txCollapsed.erase(txd->id); }
        }
        ImGui::EndPopup();
    }

    ImGui::EndTable();
    ImGui::PopStyleVar(2);

    // footer: legend + problems
    int missing = 0, mismatch = 0;
    for (auto& d : devs) for (auto& e : d.routes) { missing += e.status == RS_NO_SOURCE; mismatch += e.status == RS_RATE_MISMATCH; }
    ImGui::Spacing();
    auto legend = [&](ImU32 col, const char* txt) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + h - 4, p.y + h - 2), col, 2);
        ImGui::Dummy(ImVec2(h, h)); ImGui::SameLine(); ImGui::TextUnformatted(txt); ImGui::SameLine(0, 18 * g_scale);
    };
    legend(kGreen, "Recebendo"); legend(kAmber, "Sem áudio"); legend(kOrange, "Taxa diferente");
    if (missing) { ImGui::TextColored(ImVec4(0.78f, 0.2f, 0.2f, 1), "%d rota(s) com origem ausente da rede", missing); ImGui::SameLine(); }
    if (mismatch) { ImGui::TextColored(ImVec4(0.85f, 0.43f, 0.15f, 1), "%d rota(s) com taxa diferente", mismatch); ImGui::SameLine(); }
    ImGui::NewLine();
}

// ===========================================================================
// Devices / clock / configuration tabs
// ===========================================================================
void drawDevices(const std::vector<DeviceView>& devs) {
    ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##devs", 11, fl)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"Nome", "IP", "Taxa", "TX x RX", "Latência", "ASIO", "Pacotes RX", "Atrasados", "Perdidos", "Pacotes TX", "Versão"})
        ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (auto& d : devs) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.ann.name.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ipToString(d.ip).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f kHz", d.ann.sampleRate / 1000.0);
        ImGui::TableNextColumn(); ImGui::Text("%d x %d", d.ann.txChannels, d.ann.rxChannels);
        ImGui::TableNextColumn(); ImGui::Text("%.1f ms", d.ann.latencyUs / 1000.0);
        ImGui::TableNextColumn();
        if (d.ann.asioActive) ImGui::TextColored(ImVec4(0.18f, 0.62f, 0.36f, 1), "Ativo (%u)", d.ann.asioBuffer);
        else if (d.ann.testTone) ImGui::TextColored(ImVec4(0.84f, 0.65f, 0.16f, 1), "Tom de teste");
        else ImGui::TextDisabled("Parado");
        ImGui::TableNextColumn(); ImGui::Text("%u", d.ann.rxPackets);
        ImGui::TableNextColumn(); if (d.ann.rxLate) ImGui::TextColored(ImVec4(0.85f, 0.43f, 0.15f, 1), "%u", d.ann.rxLate); else ImGui::Text("0");
        ImGui::TableNextColumn(); if (d.ann.rxLost) ImGui::TextColored(ImVec4(0.78f, 0.2f, 0.2f, 1), "%u", d.ann.rxLost); else ImGui::Text("0");
        ImGui::TableNextColumn(); ImGui::Text("%u", d.ann.txPackets);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.ann.version.c_str());
    }
    ImGui::EndTable();
}

void drawClock(Discovery& disc, const std::vector<DeviceView>& devs) {
    std::string leader = "-";
    for (auto& d : devs) if (d.ann.isLeader) leader = d.ann.name;
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImGui::Text("Líder do relógio: %s", leader.c_str());
    if (g_fontBold) ImGui::PopFont();
    ImGui::TextWrapped("O líder é escolhido automaticamente (menor ID). Marque \"Líder preferido\" para fixar um computador como líder.");
    ImGui::Spacing();
    ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##clk", 5, fl)) return;
    for (const char* h : {"Dispositivo", "Papel", "Estado", "Offset", "Líder preferido"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (auto& d : devs) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.ann.name.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.ann.isLeader ? "Líder" : "Seguidor");
        ImGui::TableNextColumn();
        ImVec4 c = d.ann.clockState == CLK_LOCKED ? ImVec4(0.18f, 0.62f, 0.36f, 1) : d.ann.clockState == CLK_LOCKING ? ImVec4(0.84f, 0.65f, 0.16f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1);
        ImGui::TextColored(c, "%s", d.ann.isLeader ? "Referência" : clockStateText(d.ann.clockState));
        ImGui::TableNextColumn();
        if (d.ann.isLeader) ImGui::TextUnformatted("-"); else ImGui::Text("%+.1f us", d.ann.offsetNs / 1000.0);
        ImGui::TableNextColumn();
        bool pref = d.ann.preferredLeader != 0;
        ImGui::PushID(int(d.id & 0x7fffffff));
        if (ImGui::Checkbox("##pref", &pref)) sendU8(disc, d, CMD_SET_PRIORITY, pref ? 1 : 0);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void drawConfig(Discovery& disc, const std::vector<DeviceView>& devs) {
    if (devs.empty()) return;
    const DeviceView* d = nullptr;
    for (auto& x : devs) if (x.id == g_cfgDevice) d = &x;
    if (!d) { d = &devs[0]; g_cfgDevice = d->id; }

    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::BeginCombo("Dispositivo", d->ann.name.c_str())) {
        for (auto& x : devs) if (ImGui::Selectable((x.ann.name + "##" + std::to_string(x.id)).c_str(), x.id == d->id)) g_cfgDevice = x.id;
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::TextDisabled("%s", ipToString(d->ip).c_str());
    ImGui::Separator();

    static uint64_t editFor = 0;
    static std::string nameBuf;
    static int txCh = 32, rxCh = 32;
    if (editFor != d->id) { editFor = d->id; nameBuf = d->ann.name; txCh = d->ann.txChannels; rxCh = d->ann.rxChannels; }

    if (ImGui::BeginTable("##cfg", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, 200 * g_scale);
        ImGui::TableSetupColumn("b");

        ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Nome do dispositivo");
        ImGui::TableNextColumn(); ImGui::SetNextItemWidth(260 * g_scale); ImGui::InputText("##name", &nameBuf);
        ImGui::SameLine();
        if (ImGui::Button("Renomear")) { Writer w; w.str(nameBuf, kNameLen); disc.sendControl(*d, CMD_SET_NAME, w.buf); }
        ImGui::SameLine(); ImGui::TextDisabled("(rotas de outros PCs usam este nome)");

        ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Taxa de amostragem");
        ImGui::TableNextColumn(); ImGui::SetNextItemWidth(160 * g_scale);
        char cur[32]; snprintf(cur, sizeof(cur), "%.1f kHz", d->ann.sampleRate / 1000.0);
        if (ImGui::BeginCombo("##rate", cur)) {
            for (uint32_t r : kRates) {
                char l[32]; snprintf(l, sizeof(l), "%.1f kHz", r / 1000.0);
                if (ImGui::Selectable(l, r == d->ann.sampleRate)) sendU32(disc, *d, CMD_SET_RATE, r);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::TextDisabled("(todos os PCs roteados devem usar a mesma taxa)");

        ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Latência de rede (recepção)");
        ImGui::TableNextColumn(); ImGui::SetNextItemWidth(160 * g_scale);
        snprintf(cur, sizeof(cur), "%.0f ms", d->ann.latencyUs / 1000.0);
        if (ImGui::BeginCombo("##lat", cur)) {
            for (uint32_t us : kLatenciesUs) {
                char l[32]; snprintf(l, sizeof(l), "%u ms", us / 1000);
                if (ImGui::Selectable(l, us == d->ann.latencyUs)) sendU32(disc, *d, CMD_SET_LATENCY, us);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::TextDisabled("(aumente se \"Atrasados\" subir na aba Dispositivos)");

        ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Canais TX / RX");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(110 * g_scale); ImGui::InputInt("##tx", &txCh); ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * g_scale); ImGui::InputInt("##rxc", &rxCh); ImGui::SameLine();
        txCh = std::clamp(txCh, 1, kMaxChannels); rxCh = std::clamp(rxCh, 1, kMaxChannels);
        if (ImGui::Button("Aplicar canais")) { Writer w; w.u16(uint16_t(txCh)); w.u16(uint16_t(rxCh)); disc.sendControl(*d, CMD_SET_CHCOUNT, w.buf); }
        ImGui::SameLine(); ImGui::TextDisabled("(o REAPER precisa reabrir o driver)");

        ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Líder de relógio preferido");
        ImGui::TableNextColumn(); { bool v = d->ann.preferredLeader != 0; if (ImGui::Checkbox("##pl", &v)) sendU8(disc, *d, CMD_SET_PRIORITY, v); }

        ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Tom de teste (sem ASIO)");
        ImGui::TableNextColumn(); { bool v = d->ann.testTone != 0; if (ImGui::Checkbox("##tt", &v)) sendU8(disc, *d, CMD_SET_TESTTONE, v);
                                    ImGui::SameLine(); ImGui::TextDisabled("1 kHz nos canais ímpares, 500 Hz nos pares, -20 dBFS"); }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Limpar todas as rotas deste dispositivo")) ImGui::OpenPopup("##clr");
    if (ImGui::BeginPopupModal("##clr", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remover todas as rotas de recepção de %s?", d->ann.name.c_str());
        if (ImGui::Button("Remover")) { disc.sendControl(*d, CMD_CLEAR_ROUTES, {}); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Spacing(); ImGui::Separator();
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted("Nomes dos canais");
    if (g_fontBold) ImGui::PopFont();

    static std::map<std::string, std::string> edits;
    static std::string activeKey;
    if (ImGui::BeginTable("##names", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Transmissão (saídas do ASIO)");
        ImGui::TableSetupColumn("Recepção (entradas do ASIO)");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        const int n = std::max<int>(d->ann.txChannels, d->ann.rxChannels);
        for (int i = 0; i < n; ++i) {
            ImGui::TableNextRow();
            for (int dir = 0; dir < 2; ++dir) {
                ImGui::TableSetColumnIndex(dir);
                const int cnt = dir ? d->ann.rxChannels : d->ann.txChannels;
                if (i >= cnt) continue;
                const std::string key = std::to_string(d->id) + ":" + std::to_string(dir) + ":" + std::to_string(i);
                std::string& e = edits[key];
                if (activeKey != key) e = dir ? d->rxName(i) : d->txName(i);
                ImGui::Text("%2d", i + 1); ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputText(("##n" + key).c_str(), &e);
                if (ImGui::IsItemActive()) activeKey = key;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    Writer w; w.u8(uint8_t(dir)); w.u8(uint8_t(i)); w.str(e, kChanNameLen);
                    disc.sendControl(*d, CMD_SET_CHNAME, w.buf);
                }
                if (ImGui::IsItemDeactivated() && activeKey == key) activeKey.clear();
            }
        }
        ImGui::EndTable();
    }
}
} // namespace

// ===========================================================================
void setupStyle(float scale) {
    g_scale = scale;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        // Latin-1 (acentos) + setas
        static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2190, 0x21FF, 0x2022, 0x2022, 0 };
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.f * scale, nullptr, ranges);
        if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeuib.ttf") != INVALID_FILE_ATTRIBUTES)
            g_fontBold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 16.f * scale, nullptr, ranges);
    } else {
        ImFontConfig fc; fc.SizePixels = 13.f * scale;
        io.Fonts->AddFontDefault(&fc);
    }

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsLight(&s);
    s.WindowRounding = 0; s.FrameRounding = 4; s.GrabRounding = 4; s.TabRounding = 5; s.PopupRounding = 5;
    s.FramePadding = ImVec2(8, 4); s.ItemSpacing = ImVec2(8, 6); s.WindowPadding = ImVec2(12, 10);
    s.FrameBorderSize = 1.f;
    s.ScaleAllSizes(scale);

    const ImVec4 navy(0.11f, 0.17f, 0.32f, 1.f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = navy;
    c[ImGuiCol_TextDisabled]         = ImVec4(0.40f, 0.45f, 0.54f, 1.f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.85f, 0.86f, 0.88f, 1.f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.89f, 0.90f, 0.92f, 1.f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.93f, 0.94f, 0.95f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.62f, 0.65f, 0.70f, 1.f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.96f, 0.96f, 0.97f, 1.f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.90f, 0.92f, 0.95f, 1.f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.84f, 0.88f, 0.93f, 1.f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.76f, 0.78f, 0.82f, 1.f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.70f, 0.73f, 0.78f, 1.f);
    c[ImGuiCol_Button]               = ImVec4(0.76f, 0.79f, 0.83f, 1.f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.68f, 0.73f, 0.80f, 1.f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.56f, 0.63f, 0.73f, 1.f);
    c[ImGuiCol_Header]               = ImVec4(0.76f, 0.79f, 0.83f, 1.f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.68f, 0.73f, 0.80f, 1.f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.56f, 0.63f, 0.73f, 1.f);
    c[ImGuiCol_CheckMark]            = navy;
    c[ImGuiCol_SliderGrab]           = navy;
    c[ImGuiCol_Tab]                  = ImVec4(0.76f, 0.78f, 0.82f, 1.f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.90f, 0.91f, 0.93f, 1.f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.93f, 0.94f, 0.95f, 1.f);
    c[ImGuiCol_TabSelectedOverline]  = navy;
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.76f, 0.79f, 0.83f, 1.f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.58f, 0.61f, 0.66f, 1.f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.70f, 0.73f, 0.77f, 1.f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.93f, 0.94f, 0.95f, 1.f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.88f, 0.89f, 0.91f, 1.f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.82f, 0.83f, 0.86f, 1.f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.62f, 0.66f, 0.72f, 1.f);
    c[ImGuiCol_Separator]            = ImVec4(0.62f, 0.65f, 0.70f, 1.f);
}

void drawFrame(Discovery& disc) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("SilverLink", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    std::vector<DeviceView> devs = disc.devices();

    // Title bar
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = 38 * g_scale, w = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), IM_COL32(236, 238, 241, 255), IM_COL32(236, 238, 241, 255),
                                    IM_COL32(196, 201, 209, 255), IM_COL32(196, 201, 209, 255));
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(150, 156, 166, 255), 4 * g_scale);
        if (g_fontBold) ImGui::PushFont(g_fontBold);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * g_scale, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextUnformatted("SilverLink  CONTROLLER");
        if (g_fontBold) ImGui::PopFont();
        std::string leader = "-";
        for (auto& d : devs) if (d.ann.isLeader) leader = d.ann.name;
        char info[160]; snprintf(info, sizeof(info), "%d dispositivo(s)   |   Líder do relógio: %s   |   v%s", (int)devs.size(), leader.c_str(), kVersionString);
        ImVec2 ts = ImGui::CalcTextSize(info);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - ts.x - 14 * g_scale, p.y + (h - ts.y) * 0.5f));
        ImGui::TextUnformatted(info);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 8 * g_scale));
    }

    if (devs.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Nenhum dispositivo SilverLink encontrado.\n\n"
                           "- Verifique se o SilverLink Engine está rodando (ícone na bandeja do Windows).\n"
                           "- Confira no menu do ícone se a interface de rede escolhida é a placa cabeada.\n"
                           "- Libere as portas UDP 7700 a 7710 no firewall (o instalador já faz isso).\n"
                           "- No switch gerenciável, deixe o IGMP Snooping ativo com um Querier na VLAN.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Roteamento")) { drawMatrix(disc, devs); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Dispositivos")) { drawDevices(devs); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Relógio")) { drawClock(disc, devs); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Configurar")) { drawConfig(disc, devs); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace slink::ui
