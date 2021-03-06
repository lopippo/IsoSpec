#include <iostream>
#include "../../IsoSpec++/unity-build.cpp"

using std::cout;

int main()
{
    int isotopeNumbers[] = {2, 3};
    int atomCounts[] = {10, 10};
    double isotopeMasses[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    double isotopeProbabilities[] = {0.5, 0.5, 0.5, 0.3, 0.2};

    void* iso = setupIso(2, isotopeNumbers, atomCounts, isotopeMasses, isotopeProbabilities);

    void* p = setupIsoOrderedGenerator(
        iso,
        1000,
        1000);

    while(advanceToNextConfigurationIsoOrderedGenerator(p))
    {
        cout << "mass="<< massIsoOrderedGenerator(p) << " lprob=" <<
        lprobIsoOrderedGenerator(p) << std::endl;
    }

    deleteIsoOrderedGenerator(p);
    deleteIso(iso);

    return 0;
}
