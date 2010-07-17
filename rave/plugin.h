// -*- coding: utf-8 -*-
// Copyright (C) 2006-2010 Rosen Diankov (rdiankov@cs.cmu.edu)
//
// This file is part of OpenRAVE.
// OpenRAVE is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
/**
\htmlonly
\file   plugin.h
\brief  Provides helper functions for creating plugins. Defines all the necessary functions to export.
\endhtmlonly
*/
#ifndef OPENRAVE_PLUGIN_H
#define OPENRAVE_PLUGIN_H

#include <rave/rave.h>
#include <boost/format.hpp>

/// \brief Validated function callback for creating an interface function. No checks need to be made on the parmaeters.
///
/// \ingroup plugin_exports
/// Only use when \ref rave/plugin.h is included.
OpenRAVE::InterfaceBasePtr CreateInterfaceValidated(OpenRAVE::InterfaceType type, const std::string& name, std::istream& sinput, OpenRAVE::EnvironmentBasePtr penv);

/// \brief Validated function callback for returning a plugin's information. No checks need to be made on the parmaeters
///
/// \ingroup plugin_exports
/// Only use when \ref rave/plugin.h is included.
void GetPluginAttributesValidated(OpenRAVE::PLUGININFO& info);

/// \brief Definition of a plugin export. Requires \ref GetPluginAttributesValidated to be defined.
/// \ingroup plugin_exports
RAVE_PLUGIN_API OpenRAVE::InterfaceBasePtr OpenRAVECreateInterface(OpenRAVE::InterfaceType type, const std::string& name, const char* interfacehash, const char* envhash, OpenRAVE::EnvironmentBasePtr penv)
{
    if( strcmp(interfacehash,OpenRAVE::RaveGetInterfaceHash(type)) ) {
        throw openrave_exception(str(boost::format("bad interface %s hash")%RaveGetInterfaceName(type)),OpenRAVE::ORE_InvalidInterfaceHash);
    }
    if( !penv ) {
        throw openrave_exception("bad environment",OpenRAVE::ORE_InvalidArguments);
    }
    if( strcmp(envhash,OPENRAVE_ENVIRONMENT_HASH) ) {
        throw openrave_exception("bad environment hash",OpenRAVE::ORE_InvalidPlugin);
    }

    stringstream sinput(name);
    string interfacename;
    sinput >> interfacename;
    std::transform(interfacename.begin(), interfacename.end(), interfacename.begin(), ::tolower);    
    return CreateInterfaceValidated(type,interfacename,sinput,penv);
}

/// \brief Definition of a plugin export. Requires \ref GetPluginAttributesValidated to be defined.
/// \ingroup plugin_exports
RAVE_PLUGIN_API void OpenRAVEGetPluginAttributes(OpenRAVE::PLUGININFO* pinfo, int size, const char* infohash)
{
    if( pinfo == NULL ) {
        throw openrave_exception("bad data",OpenRAVE::ORE_InvalidArguments);
    }
    if( size != sizeof(OpenRAVE::PLUGININFO) ) {
        throw openrave_exception(str(boost::format("bad plugin info sizes %d != %d")%size%sizeof(OpenRAVE::PLUGININFO)),OpenRAVE::ORE_InvalidPlugin);
    }
    if( strcmp(infohash,OPENRAVE_PLUGININFO_HASH) ) {
        throw openrave_exception("bad plugin info hash",OpenRAVE::ORE_InvalidPlugin);
    }
    GetPluginAttributesValidated(*pinfo);
}

/// \brief Stub function to be defined by plugin that includes \ref rave/plugin.h.
/// \ingroup plugin_exports
RAVE_PLUGIN_API void DestroyPlugin();

//@}

#endif
