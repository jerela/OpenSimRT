/**
 * -----------------------------------------------------------------------------
 * Copyright 2019-2021 OpenSimRT developers.
 *
 * This file is part of OpenSimRT.
 *
 * OpenSimRT is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * OpenSimRT is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * OpenSimRT. If not, see <https://www.gnu.org/licenses/>.
 * -----------------------------------------------------------------------------
 */
#include "GRFMPrediction.h"
#include "Exception.h"
#include "GaitPhaseDetector.h"
#include "OpenSimUtils.h"
#include "Utils.h"

using namespace std;
using namespace OpenSim;
using namespace OpenSimRT;
using namespace SimTK;

GRFMPrediction::GRFMPrediction(const Model& otherModel,
                               const Parameters& aParameters,
                               GaitPhaseDetector* detector)
        : model(*otherModel.clone()), gaitPhaseDetector(detector),
          parameters(aParameters) {
    // reserve memory size for computing the mean gait direction
    gaitDirectionBuffer.setSize(parameters.directionWindowSize);

    // add station points to the model for the CoP trajectory
    heelStationR =
            new Station(model.getBodySet().get(parameters.rStationBodyName),
                        parameters.rHeelStationLocation);
    heelStationL =
            new Station(model.getBodySet().get(parameters.lStationBodyName),
                        parameters.lHeelStationLocation);
    toeStationR =
            new Station(model.getBodySet().get(parameters.rStationBodyName),
                        parameters.rToeStationLocation);
    toeStationL =
            new Station(model.getBodySet().get(parameters.lStationBodyName),
                        parameters.lToeStationLocation);
    heelStationR->setName("heel_station_point_r");
    heelStationL->setName("heel_station_point_l");
    toeStationR->setName("toe_station_point_r");
    toeStationL->setName("toe_station_point_l");
    model.addModelComponent(heelStationR.get());
    model.addModelComponent(heelStationL.get());
    model.addModelComponent(toeStationR.get());
    model.addModelComponent(toeStationL.get());

    // disable muscles, otherwise they apply passive forces
    OpenSimUtils::disableActuators(model);

    // initialize system
    state = model.initSystem();

    // define STA functions by Ren et al.
    // https://doi.org/10.1016/j.jbiomech.2008.06.001
    // NOTE: anteriorForceTransition is replaced with
    // reactionComponentTransition due to innacurate results.
    reactionComponentTransition = [&](const double& t) -> double {
        // STA functions depend on class' private member Tds. Clip return values
        // in order to not exceed the range [0,1] in case of inaccurate
        // estimation of Tds.
        return clip(std::exp(-std::pow((2.0 * t / Tds), 3)), 0.0, 1.0);
    };

    // define CoP trajectory (linear transition from heel -> metatarsal)
    // Source: https://doi.org/10.1016/j.jbiomech.2013.09.012
    copPosition = [&](const double& t, const Vec3& d) -> Vec3 {
        const auto omega = 2.0 * Pi / Tss;
        // clip the scale factor if exceeds the range [0,1].
        const auto scale =
                clip(-2.0 / (3 * Pi) *
                             (sin(omega * t) - sin(2 * omega * t) / 8 -
                              3.0 / 4.0 * omega * t),
                     0.0, 1.0);
        // scale the distance vector d (heel -> metatarsal)
        return scale * d;
    };
}

GRFMPrediction::Method
GRFMPrediction::selectMethod(const std::string& methodName) {
    // lsits of lower-case valid names of the input strings
    vector<string> validNENames = {"newtoneuler", "newton-euler",
                                   "newton_euler", "ne"};
    vector<string> validIDNames = {"inversedynamics", "inverse-dynamics",
                                   "inverse_dynamics", "id"};

    // find if input method exists in lists and return selected enum type
    if (find(validNENames.begin(), validNENames.end(),
             String::toLower(methodName)) != validNENames.end())
        return Method::NewtonEuler;
    else if (find(validIDNames.begin(), validIDNames.end(),
                  String::toLower(methodName)) != validIDNames.end())
        return Method::InverseDynamics;
    else
        THROW_EXCEPTION("Wrong input method. Select appropriate input name.");
}

void GRFMPrediction::computeTotalReactionComponents(const Input& input,
                                                    Vec3& totalReactionForce,
                                                    Vec3& totalReactionMoment) {
    // get matter subsystem
    const auto& matter = model.getMatterSubsystem();

    // total forces / moments
    if (parameters.method == Method::InverseDynamics) {
        // ====================================================================
        // method 1: compute total forces/moment from pelvis using ID
        // ====================================================================

        // get applied mobility (generalized) forces generated by components of
        // the model, like actuators
        const Vector& appliedMobilityForces =
                model.getMultibodySystem().getMobilityForces(state,
                                                             Stage::Dynamics);

        // get all applied body forces like those from contact
        const Vector_<SpatialVec>& appliedBodyForces =
                model.getMultibodySystem().getRigidBodyForces(state,
                                                              Stage::Dynamics);

        // perform inverse dynamics
        Vector tau;
        model.getMultibodySystem()
                .getMatterSubsystem()
                .calcResidualForceIgnoringConstraints(
                        state, appliedMobilityForces, appliedBodyForces,
                        input.qDDot, tau);

        //====================================================================
        // spatial forces/moments in pelvis wrt the ground
        Vector_<SpatialVec> spatialGenForces;
        matter.multiplyBySystemJacobian(state, tau, spatialGenForces);
        const auto& idx = model.getBodySet()
                                  .get(parameters.pelvisBodyName)
                                  .getMobilizedBodyIndex();
        totalReactionForce = spatialGenForces[idx][1];
        totalReactionMoment = spatialGenForces[idx][0];

        // ===================================================================
        // method 2: compute the reaction forces/moment based on the
        // Newton-Euler equations
        //====================================================================
    } else if (parameters.method == Method::NewtonEuler) {
        // compute body velocities and accelerations
        SimTK::Vector_<SimTK::SpatialVec> bodyVelocities;
        SimTK::Vector_<SimTK::SpatialVec> bodyAccelerations;
        matter.multiplyBySystemJacobian(state, input.qDot, bodyVelocities);
        matter.calcBodyAccelerationFromUDot(state, input.qDDot,
                                            bodyAccelerations);

        for (int i = 0; i < model.getNumBodies(); ++i) {
            const auto& body = model.getBodySet()[i];
            const auto& bix = body.getMobilizedBodyIndex();

            // F_ext
            totalReactionForce += body.getMass() * (bodyAccelerations[bix][1] -
                                                    model.getGravity());

            // M_ext
            const auto& I = body.getInertia();
            totalReactionMoment +=
                    I * bodyAccelerations[bix][0] +
                    cross(bodyVelocities[bix][0], I * bodyVelocities[bix][0]);
        }
    }
}

SimTK::Rotation
GRFMPrediction::computeGaitDirectionRotation(const std::string& bodyName) {
    const auto& body = model.getBodySet().get(bodyName);
    const auto& mob = model.getMatterSubsystem().getMobilizedBody(
            body.getMobilizedBodyIndex());

    // get body transformation
    const auto& R_GB = mob.getBodyTransform(state).R();

    // append direction to buffer (x-component in rotation matrix)
    gaitDirectionBuffer.insert((~R_GB).col(0).asVec3());

    // compute the average heading direction
    auto gaitDirection = projectionOnPlane(gaitDirectionBuffer.mean(), Vec3(0),
                                           Vec3(0, 1, 0));

    // rotation about the vertical axis to transform the reaction components
    // from the opensim global reference frame to the gait-direction
    // reference frame
    auto crossProd =
            SimTK::cross(gaitDirection, Vec3(1, 0, 0));      // |a|.|b|.sin(q).n
    auto dotProd = SimTK::dot(gaitDirection, Vec3(1, 0, 0)); // |a|.|b|.cos(q)
    auto q = std::atan(crossProd.norm() / dotProd);

    return SimTK::Rotation(q, Vec3(0, 1, 0));
}

GRFMPrediction::Output
GRFMPrediction::solve(const GRFMPrediction::Input& input) {
    Output output;
    output.t = input.t;
    output.right.force = Vec3(0.0);
    output.right.torque = Vec3(0.0);
    output.right.point = Vec3(0.0);
    output.left.force = Vec3(0.0);
    output.left.torque = Vec3(0.0);
    output.left.point = Vec3(0.0);

    if (gaitPhaseDetector->isDetectorReady()) {
        // update model state and realize state
        OpenSimUtils::updateState(model, state, input.q, input.qDot);
        model.realizeDynamics(state);

        // compute the transformation to the average heading direction
        auto R = computeGaitDirectionRotation(parameters.pelvisBodyName);

        // compute total reaction force/moment
        Vec3 totalReactionForce(0), totalReactionMoment(0);
        computeTotalReactionComponents(input, totalReactionForce,
                                       totalReactionMoment);

        // express total reaction loads in heading direction frame
        totalReactionForce = R * totalReactionForce;
        totalReactionMoment = R * totalReactionMoment;
        // totalReactionMoment[0] = 0;
        // totalReactionMoment[2] = 0;

        // time since last HS
        double time = input.t - gaitPhaseDetector->getHeelStrikeTime();
        if (time == 0.0) {
            totalForceAtThs = totalReactionForce;
            totalMomentAtThs = totalReactionMoment;
        }

        // previous double-support time period
        Tds = gaitPhaseDetector->getDoubleSupportDuration();

        // forces
        Vec3 rightReactionForce, leftReactionForce;
        seperateReactionComponents(time, totalReactionForce, totalForceAtThs,
                                   reactionComponentTransition,
                                   reactionComponentTransition,
                                   reactionComponentTransition,
                                   rightReactionForce, leftReactionForce);

        // moments
        Vec3 rightReactionMoment, leftReactionMoment;
        seperateReactionComponents(time, totalReactionMoment, totalMomentAtThs,
                                   reactionComponentTransition,
                                   reactionComponentTransition,
                                   reactionComponentTransition,
                                   rightReactionMoment, leftReactionMoment);

        // cop
        Vec3 rightPoint, leftPoint;
        computeReactionPoint(input.t, rightPoint, leftPoint);

        // results
        output.right.force = rightReactionForce;
        output.right.torque = rightReactionMoment;
        output.right.point = rightPoint;
        output.left.force = leftReactionForce;
        output.left.torque = leftReactionMoment;
        output.left.point = leftPoint;
    }
    return output;
}

void GRFMPrediction::seperateReactionComponents(
        const double& time, const Vec3& totalReactionComponent,
        const SimTK::Vec3& totalReactionAtThs,
        const TransitionFuction& anteriorComponentFunction,
        const TransitionFuction& verticalComponentFunction,
        const TransitionFuction& lateralComponentFunction,
        Vec3& rightReactionComponent, Vec3& leftReactionComponent) {
    switch (gaitPhaseDetector->getPhase()) {
    case GaitPhaseState::GaitPhase::DOUBLE_SUPPORT: {
        // compute the trailing and leading leg reaction components
        Vec3 trailingReactionComponent, leadingReactionComponent;

        // trailing leg component
        trailingReactionComponent[0] =
                totalReactionAtThs[0] * anteriorComponentFunction(time);
        trailingReactionComponent[1] =
                totalReactionAtThs[1] * verticalComponentFunction(time);
        trailingReactionComponent[2] =
                totalReactionAtThs[2] * lateralComponentFunction(time);

        // leading leg component
        leadingReactionComponent =
                totalReactionComponent - trailingReactionComponent;

        // assign to output based on the current leading/trailing leg
        switch (gaitPhaseDetector->getLeadingLeg()) {
        case GaitPhaseState::LeadingLeg::RIGHT: {
            rightReactionComponent = leadingReactionComponent;
            leftReactionComponent = trailingReactionComponent;
        } break;
        case GaitPhaseState::LeadingLeg::LEFT: {
            rightReactionComponent = trailingReactionComponent;
            leftReactionComponent = leadingReactionComponent;
        } break;
        case GaitPhaseState::LeadingLeg::INVALID: {
            cerr << "STA: invalid LeadingLeg state!" << endl;
        } break;
        }
    } break;

    case GaitPhaseState::GaitPhase::LEFT_SWING: {
        rightReactionComponent = totalReactionComponent;
        leftReactionComponent = Vec3(0);
    } break;

    case GaitPhaseState::GaitPhase::RIGHT_SWING: {
        rightReactionComponent = Vec3(0);
        leftReactionComponent = totalReactionComponent;
    } break;

    default: {
        rightReactionComponent = Vec3(0);
        leftReactionComponent = Vec3(0);
    } break;
    }
}

void GRFMPrediction::computeReactionPoint(const double& t,
                                          SimTK::Vec3& rightPoint,
                                          SimTK::Vec3& leftPoint) {
    // get previous SS time-period
    Tss = gaitPhaseDetector->getSingleSupportDuration();

    // determine gait phase
    switch (gaitPhaseDetector->getPhase()) {
    case GaitPhaseState::GaitPhase::DOUBLE_SUPPORT: {
        // first determine leading / trailing leg
        switch (gaitPhaseDetector->getLeadingLeg()) {
        case GaitPhaseState::LeadingLeg::RIGHT: {
            rightPoint = heelStationR->getLocationInGround(state);
            leftPoint = toeStationL->getLocationInGround(state);
        } break;
        case GaitPhaseState::LeadingLeg::LEFT: {
            rightPoint = toeStationR->getLocationInGround(state);
            leftPoint = heelStationL->getLocationInGround(state);
        } break;
        case GaitPhaseState::LeadingLeg::INVALID: {
            cerr << "CoP: invalid LeadingLeg state!" << endl;
        } break;
        }
    } break;

    case GaitPhaseState::GaitPhase::LEFT_SWING: {
        // distance between heel and toe station points on foot
        const auto d = toeStationR->getLocationInGround(state) -
                       heelStationR->getLocationInGround(state);

        // time since last toe-off event
        auto time = t - gaitPhaseDetector->getToeOffTime();

        // result CoP
        leftPoint = Vec3(0);
        rightPoint =
                heelStationR->getLocationInGround(state) + copPosition(time, d);
    } break;

    case GaitPhaseState::GaitPhase::RIGHT_SWING: {
        // distance between heel and toe station points on foot
        const auto d = toeStationL->getLocationInGround(state) -
                       heelStationL->getLocationInGround(state);

        // time since last toe-off event
        auto time = t - gaitPhaseDetector->getToeOffTime();

        // result CoP
        rightPoint = Vec3(0);
        leftPoint =
                heelStationL->getLocationInGround(state) + copPosition(time, d);
    } break;

    default: {
        rightPoint = Vec3(0.0);
        leftPoint = Vec3(0.0);
    } break;
    }
}
