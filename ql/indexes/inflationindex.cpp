/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2007 Chris Kenyon
 Copyright (C) 2021 Ralf Konrad Eckel

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

#include <ql/indexes/inflationindex.hpp>
#include <ql/termstructures/inflationtermstructure.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <utility>

namespace QuantLib {

    Real CPI::laggedFixing(const ext::shared_ptr<ZeroInflationIndex>& index,
                           const Date& date,
                           const Period& observationLag,
                           CPI::InterpolationType interpolationType) {

        switch (interpolationType) {
          case AsIndex: {
              return index->fixing(date - observationLag);
          }
          case Flat: {
              auto fixingPeriod = inflationPeriod(date - observationLag, index->frequency());
              return index->fixing(fixingPeriod.first);
          }
          case Linear: {
              auto fixingPeriod = inflationPeriod(date - observationLag, index->frequency());
              auto interpolationPeriod = inflationPeriod(date, index->frequency());

              if (date == interpolationPeriod.first) {
                  // special case; no interpolation.  This avoids asking for
                  // the fixing at the end of the period, which might need a
                  // forecast curve to be set.
                  return index->fixing(fixingPeriod.first);
              }

              static const auto oneDay = Period(1, Days);

              auto I0 = index->fixing(fixingPeriod.first);
              auto I1 = index->fixing(fixingPeriod.second + oneDay);

              return I0 + (I1 - I0) * (date - interpolationPeriod.first) /
                  (Real)((interpolationPeriod.second + oneDay) - interpolationPeriod.first);
          }
          default:
            QL_FAIL("unknown CPI interpolation type: " << int(interpolationType));
        }
    }


    InflationIndex::InflationIndex(std::string familyName,
                                   Region region,
                                   bool revised,
                                   bool interpolated,
                                   Frequency frequency,
                                   const Period& availabilityLag,
                                   Currency currency)
    : familyName_(std::move(familyName)), region_(std::move(region)), revised_(revised),
      interpolated_(interpolated), frequency_(frequency), availabilityLag_(availabilityLag),
      currency_(std::move(currency)) {
        name_ = region_.name() + " " + familyName_;
        registerWith(Settings::instance().evaluationDate());
        registerWith(IndexManager::instance().notifier(name()));
    }


    Calendar InflationIndex::fixingCalendar() const {
        static NullCalendar c;
        return c;
    }

    void InflationIndex::addFixing(const Date& fixingDate,
                                   Real fixing,
                                   bool forceOverwrite) {

        std::pair<Date,Date> lim = inflationPeriod(fixingDate, frequency_);
        Size n = static_cast<QuantLib::Size>(lim.second - lim.first) + 1;
        std::vector<Date> dates(n);
        std::vector<Rate> rates(n);
        for (Size i=0; i<n; ++i) {
            dates[i] = lim.first + i;
            rates[i] = fixing;
        }

        Index::addFixings(dates.begin(), dates.end(),
                          rates.begin(), forceOverwrite);
    }

    ZeroInflationIndex::ZeroInflationIndex(const std::string& familyName,
                                           const Region& region,
                                           bool revised,
                                           bool interpolated,
                                           Frequency frequency,
                                           const Period& availabilityLag,
                                           const Currency& currency,
                                           Handle<ZeroInflationTermStructure> zeroInflation)
    : InflationIndex(
          familyName, region, revised, interpolated, frequency, availabilityLag, currency),
      zeroInflation_(std::move(zeroInflation)) {
        registerWith(zeroInflation_);
    }

    Real ZeroInflationIndex::fixing(const Date& fixingDate,
                                    bool /*forecastTodaysFixing*/) const {
        if (!needsForecast(fixingDate)) {
            std::pair<Date,Date> p = inflationPeriod(fixingDate, frequency_);
            const TimeSeries<Real>& ts = timeSeries();

            Real I1 = ts[p.first];
            QL_REQUIRE(I1 != Null<Real>(),
                       "Missing " << name() << " fixing for " << p.first);

            if (interpolated_ && fixingDate > p.first) {
                Real I2 = ts[p.second+1];
                QL_REQUIRE(I2 != Null<Real>(),
                           "Missing " << name() << " fixing for " << p.second+1);

                // Use non-lagged period for interpolation
                Date observationDate = fixingDate + zeroInflation_->observationLag();
                std::pair<Date, Date> p2 = inflationPeriod(observationDate, frequency_);
                Real daysInPeriod = (p2.second + 1) - p2.first;
                Real interpolationCoefficient = (observationDate - p2.first) / daysInPeriod;

                return I1 + (I2 - I1) * interpolationCoefficient;
            } else {
                // we don't need the next fixing
                return I1;
            }
        } else {
            return forecastFixing(fixingDate);
        }
    }


    bool ZeroInflationIndex::needsForecast(const Date& fixingDate) const {

        // Stored fixings are always non-interpolated.
        // If an interpolated fixing is required then
        // the availability lag + one inflation period
        // must have passed to use historical fixings
        // (because you need the next one to interpolate).
        // The interpolation is calculated (linearly) on demand.

        Date today = Settings::instance().evaluationDate();
        Date todayMinusLag = today - availabilityLag_;

        Date historicalFixingKnown =
            inflationPeriod(todayMinusLag, frequency_).first-1;
        Date latestNeededDate = fixingDate;

        if (interpolated_) { // might need the next one too
            std::pair<Date,Date> p = inflationPeriod(fixingDate, frequency_);
            if (fixingDate > p.first)
                latestNeededDate += Period(frequency_);
        }

        if (latestNeededDate <= historicalFixingKnown) {
            // the fixing date is well before the availability lag, so
            // we know that fixings were provided.
            return false;
        } else if (latestNeededDate > today) {
            // the fixing can't be available, no matter what's in the
            // time series
            return true;
        } else {
            // we're not sure, but the fixing might be there so we
            // check.  Todo: check which fixings are not possible, to
            // avoid using fixings in the future
            Date first = Date(1, latestNeededDate.month(), latestNeededDate.year());
            Real f = timeSeries()[first];
            return (f == Null<Real>());
        }
    }


    Real ZeroInflationIndex::forecastFixing(const Date& fixingDate) const {
        // the term structure is relative to the fixing value at the base date.
        Date baseDate = zeroInflation_->baseDate();
        QL_REQUIRE(!needsForecast(baseDate),
                   name() << " index fixing at base date " << baseDate << " is not available");
        Real baseFixing = fixing(baseDate);

        std::pair<Date, Date> p = inflationPeriod(fixingDate, frequency_);

        Date firstDateInPeriod = p.first;
        Rate Z1 = zeroInflation_->zeroRate(firstDateInPeriod, Period(0,Days), false);
        Time t1 = inflationYearFraction(frequency_, interpolated_, zeroInflation_->dayCounter(),
                                        baseDate, firstDateInPeriod);
        Real I1 = baseFixing * std::pow(1.0 + Z1, t1);

        if (interpolated() && fixingDate > firstDateInPeriod) {
            Date firstDateInNextPeriod = p.second + 1;
            Rate Z2 = zeroInflation_->zeroRate(firstDateInNextPeriod, Period(0,Days), false);
            Time t2 = inflationYearFraction(frequency_, interpolated_, zeroInflation_->dayCounter(),
                                            baseDate, firstDateInNextPeriod);
            Real I2 = baseFixing * std::pow(1.0 + Z2, t2);

            // // Use non-lagged period for interpolation
            Date observationDate = fixingDate + zeroInflation_->observationLag();
            std::pair<Date, Date> p2 = inflationPeriod(observationDate, frequency_);
            Real daysInPeriod = (p2.second + 1) - p2.first;
            Real interpolationCoefficient = (observationDate - p2.first) / daysInPeriod;

            return I1 + (I2 - I1) * interpolationCoefficient;
        } else {
            return I1;
        }
    }


    ext::shared_ptr<ZeroInflationIndex> ZeroInflationIndex::clone(
                          const Handle<ZeroInflationTermStructure>& h) const {
        return ext::make_shared<ZeroInflationIndex>(
                      familyName_, region_, revised_,
                                             interpolated_, frequency_,
                                             availabilityLag_, currency_, h);
    }

    // these still need to be fixed to latest versions

    YoYInflationIndex::YoYInflationIndex(const std::string& familyName,
                                         const Region& region,
                                         bool revised,
                                         bool interpolated,
                                         bool ratio,
                                         Frequency frequency,
                                         const Period& availabilityLag,
                                         const Currency& currency,
                                         Handle<YoYInflationTermStructure> yoyInflation)
    : InflationIndex(
          familyName, region, revised, interpolated, frequency, availabilityLag, currency),
      ratio_(ratio), yoyInflation_(std::move(yoyInflation)) {
        registerWith(yoyInflation_);
    }


    Rate YoYInflationIndex::fixing(const Date& fixingDate,
                                   bool /*forecastTodaysFixing*/) const {

        Date today = Settings::instance().evaluationDate();
        Date todayMinusLag = today - availabilityLag_;
        std::pair<Date,Date> lim = inflationPeriod(todayMinusLag, frequency_);
        Date lastFix = lim.first-1;

        Date flatMustForecastOn = lastFix+1;
        Date interpMustForecastOn = lastFix+1 - Period(frequency_);


        if (interpolated() && fixingDate >= interpMustForecastOn) {
            return forecastFixing(fixingDate);
        }

        if (!interpolated() && fixingDate >= flatMustForecastOn) {
            return forecastFixing(fixingDate);
        }

        // four cases with ratio() and interpolated()

        const TimeSeries<Real>& ts = timeSeries();
        if (ratio()) {

            if(interpolated()){ // IS ratio, IS interpolated

                std::pair<Date,Date> lim = inflationPeriod(fixingDate, frequency_);
                Date fixMinus1Y = NullCalendar().advance(fixingDate, -1*Years, ModifiedFollowing);
                std::pair<Date,Date> limBef = inflationPeriod(fixMinus1Y, frequency_);
                Real dp = lim.second + 1 - lim.first;
                Real dpBef = limBef.second + 1 - limBef.first;
                Real dl = fixingDate-lim.first;
                // potentially does not work on 29th Feb
                Real dlBef = fixMinus1Y - limBef.first;
                // get the four relevant fixings
                Rate limFirstFix = ts[lim.first];
                QL_REQUIRE(limFirstFix != Null<Rate>(),
                           "Missing " << name() << " fixing for " << lim.first );
                Rate limSecondFix = ts[lim.second+1];
                QL_REQUIRE(limSecondFix != Null<Rate>(),
                           "Missing " << name() << " fixing for " << lim.second+1 );
                Rate limBefFirstFix = ts[limBef.first];
                QL_REQUIRE(limBefFirstFix != Null<Rate>(),
                           "Missing " << name() << " fixing for " << limBef.first );
                Rate limBefSecondFix = ts[limBef.second+1];
                QL_REQUIRE(limBefSecondFix != Null<Rate>(),
                           "Missing " << name() << " fixing for " << limBef.second+1 );

                Real linearNow = limFirstFix + (limSecondFix-limFirstFix)*dl/dp;
                Real linearBef = limBefFirstFix + (limBefSecondFix-limBefFirstFix)*dlBef/dpBef;
                Rate wasYES = linearNow / linearBef - 1.0;

                return wasYES;

            } else {    // IS ratio, NOT interpolated
                std::pair<Date,Date> lim = inflationPeriod(fixingDate, frequency_);
                Rate pastFixing = ts[lim.first];
                QL_REQUIRE(pastFixing != Null<Rate>(),
                            "Missing " << name() << " fixing for " << fixingDate);
                Date previousDate = fixingDate - 1*Years;
                std::pair<Date,Date> limBef = inflationPeriod(previousDate, frequency_);
                Rate previousFixing = ts[limBef.first];
                QL_REQUIRE(previousFixing != Null<Rate>(),
                           "Missing " << name() << " fixing for " << limBef.first );

                return pastFixing/previousFixing - 1.0;
            }

        } else {  // NOT ratio

            if (interpolated()) { // NOT ratio, IS interpolated

                std::pair<Date,Date> lim = inflationPeriod(fixingDate, frequency_);
                Real dp = lim.second + 1 - lim.first;
                Real dl = fixingDate - lim.first;
                Rate limFirstFix = ts[lim.first];
                QL_REQUIRE(limFirstFix != Null<Rate>(),
                            "Missing " << name() << " fixing for "
                            << lim.first );
                Rate limSecondFix = ts[lim.second+1];
                QL_REQUIRE(limSecondFix != Null<Rate>(),
                            "Missing " << name() << " fixing for "
                            << lim.second+1 );
                Real linearNow = limFirstFix + (limSecondFix-limFirstFix)*dl/dp;

                return linearNow;

            } else { // NOT ratio, NOT interpolated
                     // so just flat

                std::pair<Date,Date> lim = inflationPeriod(fixingDate, frequency_);
                Rate pastFixing = ts[lim.first];
                QL_REQUIRE(pastFixing != Null<Rate>(),
                           "Missing " << name() << " fixing for " << lim.first);
                return pastFixing;

            }
        }
    }


    Real YoYInflationIndex::forecastFixing(const Date& fixingDate) const {

        Date d;
        if (interpolated()) {
            d = fixingDate;
        } else {
            // if the value is not interpolated use the starting value
            // by internal convention this will be consistent
            std::pair<Date,Date> lim = inflationPeriod(fixingDate, frequency_);
            d = lim.first;
        }
        return yoyInflation_->yoyRate(d,0*Days);
    }

    ext::shared_ptr<YoYInflationIndex> YoYInflationIndex::clone(
                           const Handle<YoYInflationTermStructure>& h) const {
        return ext::make_shared<YoYInflationIndex>(
                      familyName_, region_, revised_,
                                            interpolated_, ratio_, frequency_,
                                            availabilityLag_, currency_, h);
    }


    CPI::InterpolationType
    detail::CPI::effectiveInterpolationType(const ext::shared_ptr<ZeroInflationIndex>& index,
                                            const QuantLib::CPI::InterpolationType& type) {
        if (type == QuantLib::CPI::AsIndex) {
            return index->interpolated() ? QuantLib::CPI::Linear : QuantLib::CPI::Flat;
        } else {
            return type;
        }
    }
}
