#include "TestFramework.h"

#include <cstring>

// Usage: CoreTests [substring-filter]
int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failedCases = 0;

    for (auto& t : rmtest::registry())
    {
        if (filter != nullptr && std::strstr (t.name, filter) == nullptr)
            continue;

        ++run;
        const int before = rmtest::failureCount();
        std::cout << "[ RUN  ] " << t.name << "\n";

        try
        {
            t.fn();
        }
        catch (const rmtest::RequireFailed&)
        {
        }
        catch (const std::exception& e)
        {
            rmtest::reportFailure (t.file, 0, std::string ("uncaught exception: ") + e.what());
        }

        if (rmtest::failureCount() != before)
            ++failedCases;
    }

    std::cout << "\n" << run << " test cases, " << failedCases << " failed, " << rmtest::failureCount() << " failed checks\n";
    return failedCases == 0 ? 0 : 1;
}
