#pragma once

// Initierar PMU (AXP2101) och slår på den matning som behövs för modemet.
// Returnerar true om init lyckades, annars false.
bool powerInit();