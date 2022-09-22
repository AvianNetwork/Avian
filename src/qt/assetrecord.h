// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2022 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_QT_ASSETRECORD_H
#define AVIAN_QT_ASSETRECORD_H

#include "math.h"
#include "amount.h"
#include "tinyformat.h"


/** UI model for unspent assets.
 */
class AssetRecord
{
public:

    AssetRecord():
            name(""), quantity(0), units(0), fIsAdministrator(false), ipfshash(""), ansID("")
    {
    }

    AssetRecord(const std::string _name, const CAmount& _quantity, const int _units, const bool _fIsAdministrator, const std::string _ipfshash, const std::string _ansID):
            name(_name), quantity(_quantity), units(_units), fIsAdministrator(_fIsAdministrator), ipfshash(_ipfshash), ansID(_ansID)
    {
    }

    std::string formattedQuantity() const {
        bool sign = quantity < 0;
        int64_t n_abs = (sign ? -quantity : quantity);
        int64_t quotient = n_abs / COIN;
        int64_t remainder = n_abs % COIN;
        remainder = remainder / pow(10, 8 - units);

        if (remainder == 0) {
            return strprintf("%s%d", sign ? "-" : "", quotient);
        }
        else {
            return strprintf("%s%d.%0" + std::to_string(units) + "d", sign ? "-" : "", quotient, remainder);
        }
    }

    /** @name Immutable attributes
      @{*/
    std::string name;
    CAmount quantity;
    int units;
    bool fIsAdministrator;
    std::string ipfshash;
    std::string ansID;
    /**@}*/

};

#endif // AVIAN_QT_ASSETRECORD_H
