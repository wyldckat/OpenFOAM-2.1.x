/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

Application
    reconstructPar

Description
    Reconstructs a mesh and fields of a case that is decomposed for parallel
    execution of OpenFOAM.

\*---------------------------------------------------------------------------*/

#include "argList.H"
#include "timeSelector.H"

#include "fvCFD.H"
#include "IOobjectList.H"
#include "processorMeshes.H"
#include "fvFieldReconstructor.H"
#include "pointFieldReconstructor.H"
#include "reconstructLagrangian.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

int main(int argc, char *argv[])
{
    // enable -constant ... if someone really wants it
    // enable -zeroTime to prevent accidentally trashing the initial fields
    timeSelector::addOptions(true, true);
    argList::noParallel();
#   include "addRegionOption.H"
    argList::addOption
    (
        "fields",
        "list",
        "specify a list of fields to be reconstructed. Eg, '(U T p)' - "
        "regular expressions not currently supported"
    );
    argList::addOption
    (
        "lagrangianFields",
        "list",
        "specify a list of lagrangian fields to be reconstructed. Eg, '(U d)' -"
        "regular expressions not currently supported, "
        "positions always included."
    );
    argList::addBoolOption
    (
        "noLagrangian",
        "skip reconstructing lagrangian positions and fields"
    );

#   include "setRootCase.H"
#   include "createTime.H"

    HashSet<word> selectedFields;
    if (args.optionFound("fields"))
    {
        args.optionLookup("fields")() >> selectedFields;
    }

    const bool noLagrangian = args.optionFound("noLagrangian");

    HashSet<word> selectedLagrangianFields;
    if (args.optionFound("lagrangianFields"))
    {
        if (noLagrangian)
        {
            FatalErrorIn(args.executable())
                << "Cannot specify noLagrangian and lagrangianFields "
                << "options together."
                << exit(FatalError);
        }

        args.optionLookup("lagrangianFields")() >> selectedLagrangianFields;
    }

    // determine the processor count directly
    label nProcs = 0;
    while (isDir(args.path()/(word("processor") + name(nProcs))))
    {
        ++nProcs;
    }

    if (!nProcs)
    {
        FatalErrorIn(args.executable())
            << "No processor* directories found"
            << exit(FatalError);
    }

    // Create the processor databases
    PtrList<Time> databases(nProcs);

    forAll(databases, procI)
    {
        databases.set
        (
            procI,
            new Time
            (
                Time::controlDictName,
                args.rootPath(),
                args.caseName()/fileName(word("processor") + name(procI))
            )
        );
    }

    // use the times list from the master processor
    // and select a subset based on the command-line options
    instantList timeDirs = timeSelector::select
    (
        databases[0].times(),
        args
    );

    if (timeDirs.empty())
    {
        FatalErrorIn(args.executable())
            << "No times selected"
            << exit(FatalError);
    }

#   include "createNamedMesh.H"
    word regionDir = word::null;
    if (regionName != fvMesh::defaultRegion)
    {
        regionDir = regionName;
    }

    // Set all times on processor meshes equal to reconstructed mesh
    forAll(databases, procI)
    {
        databases[procI].setTime(runTime.timeName(), runTime.timeIndex());
    }

    // Read all meshes and addressing to reconstructed mesh
    processorMeshes procMeshes(databases, regionName);


    // check face addressing for meshes that have been decomposed
    // with a very old foam version
#   include "checkFaceAddressingComp.H"

    // Loop over all times
    forAll(timeDirs, timeI)
    {
        // Set time for global database
        runTime.setTime(timeDirs[timeI], timeI);

        Info<< "Time = " << runTime.timeName() << endl << endl;

        // Set time for all databases
        forAll(databases, procI)
        {
            databases[procI].setTime(timeDirs[timeI], timeI);
        }

        // Check if any new meshes need to be read.
        fvMesh::readUpdateState meshStat = mesh.readUpdate();

        fvMesh::readUpdateState procStat = procMeshes.readUpdate();

        if (procStat == fvMesh::POINTS_MOVED)
        {
            // Reconstruct the points for moving mesh cases and write them out
            procMeshes.reconstructPoints(mesh);
        }
        else if (meshStat != procStat)
        {
            WarningIn(args.executable())
                << "readUpdate for the reconstructed mesh:" << meshStat << nl
                << "readUpdate for the processor meshes  :" << procStat << nl
                << "These should be equal or your addressing"
                << " might be incorrect."
                << " Please check your time directories for any "
                << "mesh directories." << endl;
        }


        // Get list of objects from processor0 database
        IOobjectList objects(procMeshes.meshes()[0], databases[0].timeName());

        {
            // If there are any FV fields, reconstruct them
            Info<< "Reconstructing FV fields" << nl << endl;

            fvFieldReconstructor fvReconstructor
            (
                mesh,
                procMeshes.meshes(),
                procMeshes.faceProcAddressing(),
                procMeshes.cellProcAddressing(),
                procMeshes.boundaryProcAddressing()
            );

            fvReconstructor.reconstructFvVolumeInternalFields<scalar>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvVolumeInternalFields<vector>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvVolumeInternalFields<sphericalTensor>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvVolumeInternalFields<symmTensor>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvVolumeInternalFields<tensor>
            (
                objects,
                selectedFields
            );

            fvReconstructor.reconstructFvVolumeFields<scalar>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvVolumeFields<vector>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvVolumeFields<sphericalTensor>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvVolumeFields<symmTensor>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvVolumeFields<tensor>
            (
                objects,
                selectedFields
            );

            fvReconstructor.reconstructFvSurfaceFields<scalar>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvSurfaceFields<vector>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvSurfaceFields<sphericalTensor>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvSurfaceFields<symmTensor>
            (
                objects,
                selectedFields
            );
            fvReconstructor.reconstructFvSurfaceFields<tensor>
            (
                objects,
                selectedFields
            );

            if (fvReconstructor.nReconstructed() == 0)
            {
                Info<< "No FV fields" << nl << endl;
            }
        }

        {
            Info<< "Reconstructing point fields" << nl << endl;

            pointMesh pMesh(mesh);
            PtrList<pointMesh> pMeshes(procMeshes.meshes().size());

            forAll(pMeshes, procI)
            {
                pMeshes.set(procI, new pointMesh(procMeshes.meshes()[procI]));
            }

            pointFieldReconstructor pointReconstructor
            (
                pMesh,
                pMeshes,
                procMeshes.pointProcAddressing(),
                procMeshes.boundaryProcAddressing()
            );

            pointReconstructor.reconstructFields<scalar>
            (
                objects,
                selectedFields
            );
            pointReconstructor.reconstructFields<vector>
            (
                objects,
                selectedFields
            );
            pointReconstructor.reconstructFields<sphericalTensor>
            (
                objects,
                selectedFields
            );
            pointReconstructor.reconstructFields<symmTensor>
            (
                objects,
                selectedFields
            );
            pointReconstructor.reconstructFields<tensor>
            (
                objects,
                selectedFields
            );

            if (pointReconstructor.nReconstructed() == 0)
            {
                Info<< "No point fields" << nl << endl;
            }
        }


        // If there are any clouds, reconstruct them.
        // The problem is that a cloud of size zero will not get written so
        // in pass 1 we determine the cloud names and per cloud name the
        // fields. Note that the fields are stored as IOobjectList from
        // the first processor that has them. They are in pass2 only used
        // for name and type (scalar, vector etc).

        if (!noLagrangian)
        {
            HashTable<IOobjectList> cloudObjects;

            forAll(databases, procI)
            {
                fileNameList cloudDirs
                (
                    readDir
                    (
                        databases[procI].timePath() / regionDir / cloud::prefix,
                        fileName::DIRECTORY
                    )
                );

                forAll(cloudDirs, i)
                {
                    // Check if we already have cloud objects for this cloudname
                    HashTable<IOobjectList>::const_iterator iter =
                        cloudObjects.find(cloudDirs[i]);

                    if (iter == cloudObjects.end())
                    {
                        // Do local scan for valid cloud objects
                        IOobjectList sprayObjs
                        (
                            procMeshes.meshes()[procI],
                            databases[procI].timeName(),
                            cloud::prefix/cloudDirs[i]
                        );

                        IOobject* positionsPtr = sprayObjs.lookup("positions");

                        if (positionsPtr)
                        {
                            cloudObjects.insert(cloudDirs[i], sprayObjs);
                        }
                    }
                }
            }


            if (cloudObjects.size())
            {
                // Pass2: reconstruct the cloud
                forAllConstIter(HashTable<IOobjectList>, cloudObjects, iter)
                {
                    const word cloudName = string::validate<word>(iter.key());

                    // Objects (on arbitrary processor)
                    const IOobjectList& sprayObjs = iter();

                    Info<< "Reconstructing lagrangian fields for cloud "
                        << cloudName << nl << endl;

                    reconstructLagrangianPositions
                    (
                        mesh,
                        cloudName,
                        procMeshes.meshes(),
                        procMeshes.faceProcAddressing(),
                        procMeshes.cellProcAddressing()
                    );
                    reconstructLagrangianFields<label>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFieldFields<label>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFields<scalar>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFieldFields<scalar>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFields<vector>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFieldFields<vector>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFields<sphericalTensor>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFieldFields<sphericalTensor>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFields<symmTensor>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFieldFields<symmTensor>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFields<tensor>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                    reconstructLagrangianFieldFields<tensor>
                    (
                        cloudName,
                        mesh,
                        procMeshes.meshes(),
                        sprayObjs,
                        selectedLagrangianFields
                    );
                }
            }
            else
            {
                Info<< "No lagrangian fields" << nl << endl;
            }
        }

        // If there are any "uniform" directories copy them from
        // the master processor

        fileName uniformDir0 = databases[0].timePath()/"uniform";
        if (isDir(uniformDir0))
        {
            cp(uniformDir0, runTime.timePath());
        }
    }

    Info<< "End.\n" << endl;

    return 0;
}


// ************************************************************************* //
