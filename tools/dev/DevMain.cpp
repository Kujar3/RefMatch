#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

// RefMatchDev: developer tool built from the same sources as the plugin.
//   RefMatchDev selftest                      -> processor-level integration tests, exit code 0 on success
//   RefMatchDev snapshot <outDir>             -> renders the editor in several states to PNG files in outDir
//   RefMatchDev compare <mix> <ref> [outDir]  -> plays <mix> through the processor against reference <ref> like a
//                                                host would, prints the readouts + hints, optionally snapshots

int runSelfTest();                                                                               // SelfTest.cpp
int runSnapshot (const juce::File& outputDir);                                                   // Snapshot.cpp
int runCompare (const juce::File& mix, const juce::File& reference, const juce::File& outDir);   // Compare.cpp

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::String command = argc > 1 ? juce::String (argv[1]) : juce::String();
    const auto cwd = juce::File::getCurrentWorkingDirectory();

    if (command == "selftest")
        return runSelfTest();

    if (command == "snapshot")
        return runSnapshot (cwd.getChildFile (argc > 2 ? argv[2] : "snapshots"));

    if (command == "compare" && argc > 3)
        return runCompare (cwd.getChildFile (argv[2]), cwd.getChildFile (argv[3]),
                           argc > 4 ? cwd.getChildFile (argv[4]) : juce::File());

    std::cout << "usage: RefMatchDev selftest | snapshot <outDir> | compare <mix> <reference> [outDir]\n";
    return 2;
}
