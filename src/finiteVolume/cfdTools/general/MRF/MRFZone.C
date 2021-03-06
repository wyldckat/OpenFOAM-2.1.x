/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2012 OpenFOAM Foundation
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

\*---------------------------------------------------------------------------*/

#include "MRFZone.H"
#include "fvMesh.H"
#include "volFields.H"
#include "surfaceFields.H"
#include "fvMatrices.H"
#include "syncTools.H"
#include "faceSet.H"
#include "geometricOneField.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

defineTypeNameAndDebug(Foam::MRFZone, 0);


// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

void Foam::MRFZone::setMRFFaces()
{
    const polyBoundaryMesh& patches = mesh_.boundaryMesh();

    // Type per face:
    //  0:not in zone
    //  1:moving with frame
    //  2:other
    labelList faceType(mesh_.nFaces(), 0);

    // Determine faces in cell zone
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // (without constructing cells)

    const labelList& own = mesh_.faceOwner();
    const labelList& nei = mesh_.faceNeighbour();

    // Cells in zone
    boolList zoneCell(mesh_.nCells(), false);

    if (cellZoneID_ != -1)
    {
        const labelList& cellLabels = mesh_.cellZones()[cellZoneID_];
        forAll(cellLabels, i)
        {
            zoneCell[cellLabels[i]] = true;
        }
    }


    label nZoneFaces = 0;

    for (label faceI = 0; faceI < mesh_.nInternalFaces(); faceI++)
    {
        if (zoneCell[own[faceI]] || zoneCell[nei[faceI]])
        {
            faceType[faceI] = 1;
            nZoneFaces++;
        }
    }


    labelHashSet excludedPatches(excludedPatchLabels_);

    forAll(patches, patchI)
    {
        const polyPatch& pp = patches[patchI];

        if (pp.coupled() || excludedPatches.found(patchI))
        {
            forAll(pp, i)
            {
                label faceI = pp.start()+i;

                if (zoneCell[own[faceI]])
                {
                    faceType[faceI] = 2;
                    nZoneFaces++;
                }
            }
        }
        else if (!isA<emptyPolyPatch>(pp))
        {
            forAll(pp, i)
            {
                label faceI = pp.start()+i;

                if (zoneCell[own[faceI]])
                {
                    faceType[faceI] = 1;
                    nZoneFaces++;
                }
            }
        }
    }

    // Now we have for faceType:
    //  0   : face not in cellZone
    //  1   : internal face or normal patch face
    //  2   : coupled patch face or excluded patch face

    // Sort into lists per patch.

    internalFaces_.setSize(mesh_.nFaces());
    label nInternal = 0;

    for (label faceI = 0; faceI < mesh_.nInternalFaces(); faceI++)
    {
        if (faceType[faceI] == 1)
        {
            internalFaces_[nInternal++] = faceI;
        }
    }
    internalFaces_.setSize(nInternal);

    labelList nIncludedFaces(patches.size(), 0);
    labelList nExcludedFaces(patches.size(), 0);

    forAll(patches, patchi)
    {
        const polyPatch& pp = patches[patchi];

        forAll(pp, patchFacei)
        {
            label faceI = pp.start()+patchFacei;

            if (faceType[faceI] == 1)
            {
                nIncludedFaces[patchi]++;
            }
            else if (faceType[faceI] == 2)
            {
                nExcludedFaces[patchi]++;
            }
        }
    }

    includedFaces_.setSize(patches.size());
    excludedFaces_.setSize(patches.size());
    forAll(nIncludedFaces, patchi)
    {
        includedFaces_[patchi].setSize(nIncludedFaces[patchi]);
        excludedFaces_[patchi].setSize(nExcludedFaces[patchi]);
    }
    nIncludedFaces = 0;
    nExcludedFaces = 0;

    forAll(patches, patchi)
    {
        const polyPatch& pp = patches[patchi];

        forAll(pp, patchFacei)
        {
            label faceI = pp.start()+patchFacei;

            if (faceType[faceI] == 1)
            {
                includedFaces_[patchi][nIncludedFaces[patchi]++] = patchFacei;
            }
            else if (faceType[faceI] == 2)
            {
                excludedFaces_[patchi][nExcludedFaces[patchi]++] = patchFacei;
            }
        }
    }


    if (debug)
    {
        faceSet internalFaces(mesh_, "internalFaces", internalFaces_);
        Pout<< "Writing " << internalFaces.size()
            << " internal faces in MRF zone to faceSet "
            << internalFaces.name() << endl;
        internalFaces.write();

        faceSet MRFFaces(mesh_, "includedFaces", 100);
        forAll(includedFaces_, patchi)
        {
            forAll(includedFaces_[patchi], i)
            {
                label patchFacei = includedFaces_[patchi][i];
                MRFFaces.insert(patches[patchi].start()+patchFacei);
            }
        }
        Pout<< "Writing " << MRFFaces.size()
            << " patch faces in MRF zone to faceSet "
            << MRFFaces.name() << endl;
        MRFFaces.write();

        faceSet excludedFaces(mesh_, "excludedFaces", 100);
        forAll(excludedFaces_, patchi)
        {
            forAll(excludedFaces_[patchi], i)
            {
                label patchFacei = excludedFaces_[patchi][i];
                excludedFaces.insert(patches[patchi].start()+patchFacei);
            }
        }
        Pout<< "Writing " << excludedFaces.size()
            << " faces in MRF zone with special handling to faceSet "
            << excludedFaces.name() << endl;
        excludedFaces.write();
    }
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::MRFZone::MRFZone(const fvMesh& mesh, Istream& is)
:
    mesh_(mesh),
    name_(is),
    dict_(is),
    cellZoneID_(mesh_.cellZones().findZoneID(name_)),
    excludedPatchNames_
    (
        dict_.lookupOrDefault("nonRotatingPatches", wordList(0))
    ),
    origin_(dict_.lookup("origin")),
    axis_(dict_.lookup("axis")),
    omega_(dict_.lookup("omega")),
    Omega_("Omega", omega_*axis_)
{
    if (dict_.found("patches"))
    {
        WarningIn("MRFZone(const fvMesh&, Istream&)")
            << "Ignoring entry 'patches'\n"
            << "    By default all patches within the rotating region rotate.\n"
            << "    Optionally supply excluded patches "
            << "using 'nonRotatingPatches'."
            << endl;
    }

    const polyBoundaryMesh& patches = mesh_.boundaryMesh();

    axis_ = axis_/mag(axis_);
    Omega_ = omega_*axis_;

    excludedPatchLabels_.setSize(excludedPatchNames_.size());

    forAll(excludedPatchNames_, i)
    {
        excludedPatchLabels_[i] = patches.findPatchID(excludedPatchNames_[i]);

        if (excludedPatchLabels_[i] == -1)
        {
            FatalErrorIn
            (
                "Foam::MRFZone::MRFZone(const fvMesh&, Istream&)"
            )   << "cannot find MRF patch " << excludedPatchNames_[i]
                << exit(FatalError);
        }
    }


    bool cellZoneFound = (cellZoneID_ != -1);
    reduce(cellZoneFound, orOp<bool>());

    if (!cellZoneFound)
    {
        FatalErrorIn
        (
            "Foam::MRFZone::MRFZone(const fvMesh&, Istream&)"
        )   << "cannot find MRF cellZone " << name_
            << exit(FatalError);
    }

    setMRFFaces();
}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void Foam::MRFZone::addCoriolis
(
    const volVectorField& U,
    volVectorField& ddtU
) const
{
    if (cellZoneID_ == -1)
    {
        return;
    }

    const labelList& cells = mesh_.cellZones()[cellZoneID_];
    const scalarField& V = mesh_.V();
    vectorField& ddtUc = ddtU.internalField();
    const vectorField& Uc = U.internalField();
    const vector& Omega = Omega_.value();

    forAll(cells, i)
    {
        label celli = cells[i];
        ddtUc[celli] += V[celli]*(Omega ^ Uc[celli]);
    }
}


void Foam::MRFZone::addCoriolis(fvVectorMatrix& UEqn) const
{
    if (cellZoneID_ == -1)
    {
        return;
    }

    const labelList& cells = mesh_.cellZones()[cellZoneID_];
    const scalarField& V = mesh_.V();
    vectorField& Usource = UEqn.source();
    const vectorField& U = UEqn.psi();
    const vector& Omega = Omega_.value();

    forAll(cells, i)
    {
        label celli = cells[i];
        Usource[celli] -= V[celli]*(Omega ^ U[celli]);
    }
}


void Foam::MRFZone::addCoriolis
(
    const volScalarField& rho,
    fvVectorMatrix& UEqn
) const
{
    if (cellZoneID_ == -1)
    {
        return;
    }

    const labelList& cells = mesh_.cellZones()[cellZoneID_];
    const scalarField& V = mesh_.V();
    vectorField& Usource = UEqn.source();
    const vectorField& U = UEqn.psi();
    const vector& Omega = Omega_.value();

    forAll(cells, i)
    {
        label celli = cells[i];
        Usource[celli] -= V[celli]*rho[celli]*(Omega ^ U[celli]);
    }
}


void Foam::MRFZone::relativeVelocity(volVectorField& U) const
{
    const volVectorField& C = mesh_.C();

    const vector& origin = origin_.value();
    const vector& Omega = Omega_.value();

    const labelList& cells = mesh_.cellZones()[cellZoneID_];

    forAll(cells, i)
    {
        label celli = cells[i];
        U[celli] -= (Omega ^ (C[celli] - origin));
    }

    // Included patches
    forAll(includedFaces_, patchi)
    {
        forAll(includedFaces_[patchi], i)
        {
            label patchFacei = includedFaces_[patchi][i];
            U.boundaryField()[patchi][patchFacei] = vector::zero;
        }
    }

    // Excluded patches
    forAll(excludedFaces_, patchi)
    {
        forAll(excludedFaces_[patchi], i)
        {
            label patchFacei = excludedFaces_[patchi][i];
            U.boundaryField()[patchi][patchFacei] -=
                (Omega ^ (C.boundaryField()[patchi][patchFacei] - origin));
        }
    }
}


void Foam::MRFZone::absoluteVelocity(volVectorField& U) const
{
    const volVectorField& C = mesh_.C();

    const vector& origin = origin_.value();
    const vector& Omega = Omega_.value();

    const labelList& cells = mesh_.cellZones()[cellZoneID_];

    forAll(cells, i)
    {
        label celli = cells[i];
        U[celli] += (Omega ^ (C[celli] - origin));
    }

    // Included patches
    forAll(includedFaces_, patchi)
    {
        forAll(includedFaces_[patchi], i)
        {
            label patchFacei = includedFaces_[patchi][i];
            U.boundaryField()[patchi][patchFacei] =
                (Omega ^ (C.boundaryField()[patchi][patchFacei] - origin));
        }
    }

    // Excluded patches
    forAll(excludedFaces_, patchi)
    {
        forAll(excludedFaces_[patchi], i)
        {
            label patchFacei = excludedFaces_[patchi][i];
            U.boundaryField()[patchi][patchFacei] +=
                (Omega ^ (C.boundaryField()[patchi][patchFacei] - origin));
        }
    }
}


void Foam::MRFZone::relativeFlux(surfaceScalarField& phi) const
{
    relativeRhoFlux(geometricOneField(), phi);
}


void Foam::MRFZone::relativeFlux
(
    const surfaceScalarField& rho,
    surfaceScalarField& phi
) const
{
    relativeRhoFlux(rho, phi);
}


void Foam::MRFZone::absoluteFlux(surfaceScalarField& phi) const
{
    absoluteRhoFlux(geometricOneField(), phi);
}


void Foam::MRFZone::absoluteFlux
(
    const surfaceScalarField& rho,
    surfaceScalarField& phi
) const
{
    absoluteRhoFlux(rho, phi);
}


void Foam::MRFZone::correctBoundaryVelocity(volVectorField& U) const
{
    const vector& origin = origin_.value();
    const vector& Omega = Omega_.value();

    // Included patches
    forAll(includedFaces_, patchi)
    {
        const vectorField& patchC = mesh_.Cf().boundaryField()[patchi];

        vectorField pfld(U.boundaryField()[patchi]);

        forAll(includedFaces_[patchi], i)
        {
            label patchFacei = includedFaces_[patchi][i];

            pfld[patchFacei] = (Omega ^ (patchC[patchFacei] - origin));
        }

        U.boundaryField()[patchi] == pfld;
    }
}


Foam::Ostream& Foam::operator<<(Ostream& os, const MRFZone& MRF)
{
    os  << indent << nl;
    os.write(MRF.name_) << nl;
    os  << token::BEGIN_BLOCK << incrIndent << nl;
    os.writeKeyword("origin") << MRF.origin_ << token::END_STATEMENT << nl;
    os.writeKeyword("axis") << MRF.axis_ << token::END_STATEMENT << nl;
    os.writeKeyword("omega") << MRF.omega_ << token::END_STATEMENT << nl;

    if (MRF.excludedPatchNames_.size())
    {
        os.writeKeyword("nonRotatingPatches") << MRF.excludedPatchNames_
            << token::END_STATEMENT << nl;
    }

    os  << decrIndent << token::END_BLOCK << nl;

    return os;
}

// ************************************************************************* //
