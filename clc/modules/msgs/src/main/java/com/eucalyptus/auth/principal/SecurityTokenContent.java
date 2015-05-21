/*************************************************************************
 * Copyright 2009-2015 Eucalyptus Systems, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 * Please contact Eucalyptus Systems, Inc., 6755 Hollister Ave., Goleta
 * CA 93117, USA or visit http://www.eucalyptus.com/licenses/ if you need
 * additional information or have any questions.
 ************************************************************************/
package com.eucalyptus.auth.principal;

import javax.annotation.Nonnull;
import com.google.common.base.Optional;

/**
 * Represents the information from a security token
 *
 * <p>The token will have one originating identifier.</p>
 */
public interface SecurityTokenContent {

  @Nonnull Optional<String> getOriginatingAccessKeyId( );
  @Nonnull Optional<String> getOriginatingUserId( );
  @Nonnull Optional<String> getOriginatingRoleId( );

  @Nonnull String getNonce( );

  long getCreated( );
  long getExpires( );
}