// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

namespace load_progress
{
    // ProgressMeter owns the standalone Scaleform meter displayed by LoadingMenu. It loads
    // SkyrimLoadProgress/LoadingProgressMeter.swf into a root-level movie clip, initializes the
    // meter's Empty and Full timeline labels, and positions the asset inside the menu safe zone.
    //
    // The external movie exposes Meter_mc, a scaling-grid-aware Frame_mc, and an invisible Bounds_mc.
    // This keeps the artwork replaceable by skin authors while allowing the center of the frame and
    // animated fill to stretch without distorting the end caps. Identical assets are installed under
    // Interface and Interface/Exported so the relative request resolves from either menu location.
    class ProgressMeter final
    {
    public:
        static ProgressMeter& GetSingleton();

        void Update(RE::IMenu* a_menu, double a_percent, float a_interval);

    private:
        static void   SetNumber(RE::GFxValue&, const char*, double);
        static bool   GetNumber(const RE::GFxValue&, const char*, double&);
        static bool   ConvertGlobalPointToLocal(RE::GFxMovieView*, RE::GFxValue&, double&, double&);
        static bool   ConvertLocalPointToGlobal(RE::GFxMovieView*, RE::GFxValue&, double&, double&);
        static bool   GetClipBounds(RE::GFxValue&, RE::GFxValue&, std::array<double, 4>&);
        static bool   GetGlobalClipBounds(
              RE::GFxMovieView*, RE::GFxValue&, RE::GFxValue&, std::array<double, 4>&);
        static double CalculateMeterXScale(double, double, double);
        static bool   ApplyLayout(
              RE::GFxMovieView*, RE::GFxValue&, RE::GFxValue&, RE::GFxValue&, RE::GFxValue&, RE::GFxValue&);
        static void ApplyFade(RE::GFxMovieView*, RE::GFxValue&, float);
        static bool Create(RE::GFxMovieView*);
        static bool SetPercent(RE::GFxValue&, double);

        ProgressMeter() = default;
        ProgressMeter(const ProgressMeter&) = delete;
        ProgressMeter(ProgressMeter&&) = delete;
        ProgressMeter& operator=(const ProgressMeter&) = delete;
        ProgressMeter& operator=(ProgressMeter&&) = delete;
    };
}
