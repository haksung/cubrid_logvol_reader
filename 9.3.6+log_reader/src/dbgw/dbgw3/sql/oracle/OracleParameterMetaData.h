/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef ORACLEPARAMETERMETADATA_H_
#define ORACLEPARAMETERMETADATA_H_

namespace dbgw
{

  namespace sql
  {

    class _OracleParameterMetaData
    {
    public:
      _OracleParameterMetaData();

      void setParamType(ValueType type);
      void setParamMode(ParameterMode mode);
      void setReservedSize(int nSize);

    public:
      bool isUsed() const;
      bool isInParameter() const;
      int getSize() const;
      bool isLazyBindingOutParameter() const;
      ValueType getType() const;

    private:
      ValueType m_type;
      ParameterMode m_mode;
      int m_nSize;
    };

  }

}

#endif
