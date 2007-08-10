/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2007 Giorgio Facchinetti

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include <ql/pricingengines/bond/bondengine.hpp>
#include <ql/cashflows/cashflows.hpp>

namespace QuantLib {

    BondEngine::BondEngine(const Handle<YieldTermStructure>& discountCurve)
    : discountCurve_(discountCurve) {
        registerWith(discountCurve_);
    }

    void BondEngine::update() {
        notifyObservers();
    }

    Handle<YieldTermStructure> BondEngine::discountCurve() const {
        return discountCurve_;
    }

    void BondEngine::calculate() const {

        const Leg& cashflows = arguments_.cashflows;
        const Date& settlementDate = arguments_.settlementDate;
        
        QL_REQUIRE(!discountCurve_.empty(),"no discounting term structure set");
        results_.value = CashFlows::npv(cashflows,
                                        **discountCurve_,
                                        settlementDate, settlementDate);
    }
}